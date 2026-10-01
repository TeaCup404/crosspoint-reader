#include "HomeSync.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <OpdsParser.h>
#include <OpdsStream.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/LibraryCoverThumbs.h"
#include "Diag.h"
#if HOMESYNC
#include "Tailnet.h"
#endif
#include "util/OpdsFilename.h"
#include "util/UrlUtils.h"

namespace homesync {
namespace {

constexpr const char* STATE_FILE = "/.crosspoint/homesync.json";
constexpr const char* DEFAULT_SHELF = "To Reader";
constexpr const char* SHELF_INDEX_PATH = "/opds/shelfindex";  // Calibre-Web
constexpr const char* NEWEST_PATH = "/opds/new";              // Calibre-Web
constexpr int DEFAULT_MAX_NEW = 10;
// Away from home the library is reached over Tailscale, through Caddy.
constexpr const char* DEFAULT_TAILNET_URL = "https://books.gabyhome.xyz/opds";
constexpr const char* DEFAULT_TAILNET_IP = "100.86.140.113";
constexpr int DEFAULT_AUTO_HOURS = 6;
constexpr int MAX_FEED_PAGES = 10;
constexpr size_t MAX_REMEMBERED_IDS = 500;
constexpr time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01: older means the RTC was never set

struct State {
  std::string shelf = DEFAULT_SHELF;
  std::string feed;
  std::string tailnetUrl = DEFAULT_TAILNET_URL;
  std::string tailnetIp = DEFAULT_TAILNET_IP;
  int maxNew = DEFAULT_MAX_NEW;
  int autoHours = DEFAULT_AUTO_HOURS;
  time_t lastSync = 0;
  time_t lastAttempt = 0;
  std::vector<std::string> synced;
};

State loadState() {
  State s;
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(STATE_FILE, doc)) return s;
  s.shelf = doc["shelf"] | DEFAULT_SHELF;
  s.feed = doc["feed"] | "";
  s.tailnetUrl = doc["tailnetUrl"] | DEFAULT_TAILNET_URL;
  s.tailnetIp = doc["tailnetIp"] | DEFAULT_TAILNET_IP;
  s.maxNew = doc["maxNew"] | DEFAULT_MAX_NEW;
  s.autoHours = doc["autoHours"] | DEFAULT_AUTO_HOURS;
  s.lastSync = static_cast<time_t>(doc["lastSync"] | 0LL);
  s.lastAttempt = static_cast<time_t>(doc["lastAttempt"] | 0LL);
  for (JsonVariantConst id : doc["synced"].as<JsonArrayConst>()) {
    const char* v = id | "";
    if (*v) s.synced.emplace_back(v);
  }
  return s;
}

void saveState(const State& s) {
  JsonDocument doc;
  doc["shelf"] = s.shelf.c_str();
  doc["feed"] = s.feed.c_str();
  doc["tailnetUrl"] = s.tailnetUrl.c_str();
  doc["tailnetIp"] = s.tailnetIp.c_str();
  doc["maxNew"] = s.maxNew;
  doc["autoHours"] = s.autoHours;
  doc["lastSync"] = static_cast<long long>(s.lastSync);
  doc["lastAttempt"] = static_cast<long long>(s.lastAttempt);
  JsonArray ids = doc["synced"].to<JsonArray>();
  // Keep the newest MAX_REMEMBERED_IDS; older ones are only a re-download risk.
  const size_t start = s.synced.size() > MAX_REMEMBERED_IDS ? s.synced.size() - MAX_REMEMBERED_IDS : 0;
  for (size_t i = start; i < s.synced.size(); ++i) ids.add(s.synced[i].c_str());
  PersistableStoreBase::writeDocToFile(STATE_FILE, doc);
}

time_t nowEpoch() {
  // RTC chip when the board has one; otherwise the system clock, which keeps
  // running through deep sleep and silent reboots once SNTP has set it.
  time_t e = 0;
  struct tm t{};
  if (halClock.isAvailable() && halClock.localTime(t)) {
    e = mktime(&t);
  } else {
    e = time(nullptr);
  }
  return e >= MIN_VALID_EPOCH ? e : 0;
}

void syncClock() {
  if (halClock.isAvailable()) {
    halClock.syncFromNTP();
    return;
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 50 && time(nullptr) < MIN_VALID_EPOCH; ++i) delay(100);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

struct Feed {
  bool ok = false;
  std::vector<OpdsEntry> entries;
  std::string next;
};

// Set for the rest of a sync once the home LAN turned out to be unreachable.
bool viaTailnet = false;

Feed fetch(const OpdsServer& server, const std::string& url) {
  Feed feed;
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    bool ok;
#if HOMESYNC
    if (viaTailnet) {
      ok = tailnet::fetch(
          url,
          [&stream](const uint8_t* data, size_t len) {
            stream.write(data, len);
            return true;
          },
          server.username, server.password);
    } else
#endif
    {
      ok = HttpDownloader::fetchUrl(url, stream, server.username, server.password);
    }
    if (!ok) return feed;
  }
  if (!parser) return feed;
  feed.next = parser.getNextPageUrl();
  feed.entries = std::move(parser).getEntries();
  feed.ok = true;
  return feed;
}

}  // namespace

bool forceTailnetOnce = false;

bool autoSyncDue() {
  if (!OPDS_STORE.hasServers()) return false;
  const State s = loadState();
  if (s.autoHours <= 0) return false;
  const time_t now = nowEpoch();
  // Unknown clock: no auto-sync (it would run on every wake). A manual sync
  // sets the clock, after which auto-sync works.
  if (now == 0) return false;
  const time_t last = std::max(s.lastSync, s.lastAttempt);
  return last == 0 || now - last >= static_cast<time_t>(s.autoHours) * 3600;
}

void noteAutoAttempt() {
  State s = loadState();
  const time_t now = nowEpoch();
  if (now == 0) return;
  s.lastAttempt = now;
  saveState(s);
}

Result run(const OpdsServer& server, const std::function<void(const char*)>& status, const ProgressFn& progress,
           bool* cancel) {
  Result result;
  if (server.url.empty()) {
    result.error = "No OPDS server URL";
    return result;
  }
  State state = loadState();

  if (diag::pending()) diag::flush();

  // Keep the clock honest while we are online; the auto-sync throttle needs it.
  status("Setting clock...");
  syncClock();

  // 1. Reach the library: home LAN first, Tailscale when that fails.
  viaTailnet = false;
  std::string root = server.url;
  status("Looking for shelf...");
  const bool forced = std::exchange(forceTailnetOnce, false);
  Feed shelves;
  if (!forced) shelves = fetch(server, UrlUtils::buildUrl(root, SHELF_INDEX_PATH));
#if HOMESYNC
  if (!shelves.ok && tailnet::configured()) {
    LOG_INF("SYNC", forced ? "Forced Tailscale sync (test)" : "Home library unreachable, trying Tailscale");
    if (!tailnet::up(state.tailnetIp.c_str(), status, cancel)) {
      result.error = "Tailscale did not connect";
      return result;
    }
    viaTailnet = true;
    root = state.tailnetUrl;
    status("Looking for shelf (Tailscale)...");
    shelves = fetch(server, UrlUtils::buildUrl(root, SHELF_INDEX_PATH));
  }
#endif
  if (!shelves.ok) {
    result.error = "Library not reachable";
    return result;
  }

  // 2. Resolve which feed to sync.
  std::string feedUrl;
  int cap = 0;  // 0 = no cap
  if (!state.feed.empty()) {
    feedUrl = UrlUtils::buildUrl(root, state.feed);
    result.source = state.feed;
  } else {
    const std::string wanted = lower(state.shelf);
    if (shelves.ok) {
      for (const auto& e : shelves.entries) {
        // Calibre-Web appends a (localized) marker to some shelf titles, e.g.
        // "To Reader (Public)", so accept the name followed by " (...)".
        const std::string title = lower(e.title);
        const bool match = title == wanted || title.rfind(wanted + " (", 0) == 0;
        if (e.type == OpdsEntryType::NAVIGATION && match) {
          feedUrl = UrlUtils::buildUrl(UrlUtils::buildUrl(root, SHELF_INDEX_PATH), e.href);
          result.source = "Shelf: " + e.title;
          break;
        }
      }
    }
    if (feedUrl.empty()) {
      feedUrl = UrlUtils::buildUrl(root, NEWEST_PATH);
      cap = state.maxNew > 0 ? state.maxNew : DEFAULT_MAX_NEW;
      result.source = "Newest " + std::to_string(cap) + " books";
    }
  }
  bool tunnelled = viaTailnet;
#if HOMESYNC
  tunnelled = tunnelled || tailnet::isUp();  // HttpDownloader may have routed the LAN URLs
#endif
  if (tunnelled) result.source += " via Tailscale";
  LOG_INF("SYNC", "Syncing %s (%s)", feedUrl.c_str(), result.source.c_str());

  // 3. Walk the feed (following pagination) and collect books not on the device.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) haveFolder = false;
  const auto fmt = static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat);

  struct Todo {
    std::string id, title, url, path;
  };
  std::vector<Todo> todo;
  int seen = 0;
  std::string pageUrl = feedUrl;
  for (int page = 0; page < MAX_FEED_PAGES && !pageUrl.empty(); ++page) {
    if (cancel && *cancel) break;
    status("Reading feed...");
    Feed feed = fetch(server, pageUrl);
    if (!feed.ok) {
      if (page == 0) {
        result.error = "Could not read the feed";
        return result;
      }
      break;
    }
    for (const auto& e : feed.entries) {
      if (e.type != OpdsEntryType::BOOK || e.href.empty()) continue;
      if (cap > 0 && seen >= cap) break;
      ++seen;
      const std::string key = e.id.empty() ? e.href : e.id;
      std::string path;
      if (haveFolder) path += folder;
      path += '/';
      path += opdsBookFilename(e.author, e.title, fmt);
      const bool known = std::find(state.synced.begin(), state.synced.end(), key) != state.synced.end();
      if (known || Storage.exists(path.c_str())) {
        if (!known) state.synced.push_back(key);  // on the card already: remember it
        ++result.alreadyHere;
        continue;
      }
      todo.push_back(Todo{key, e.title, UrlUtils::buildUrl(pageUrl, e.href), std::move(path)});
    }
    if (cap > 0 && seen >= cap) break;
    pageUrl = feed.next.empty() ? "" : UrlUtils::buildUrl(pageUrl, feed.next);
  }

  // 4. Download.
  for (size_t i = 0; i < todo.size(); ++i) {
    if (cancel && *cancel) break;
    const Todo& t = todo[i];
    if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
        ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
      LOG_ERR("SYNC", "Low heap (%u free), stopping", ESP.getFreeHeap());
      result.error = "Low memory";
      break;
    }
    Progress p;
    p.index = static_cast<int>(i) + 1;
    p.total = static_cast<int>(todo.size());
    p.title = t.title;
    progress(p);
    const HttpDownloader::ProgressCallback onChunk = [&](const size_t done, const size_t total) {
      p.bytes = done;
      p.totalBytes = total;
      progress(p);
    };
#if HOMESYNC
    const auto rc = viaTailnet
                        ? tailnet::download(t.url, t.path, onChunk, cancel, server.username, server.password)
                        : HttpDownloader::downloadToFile(t.url, t.path, onChunk, cancel, server.username, server.password);
#else
    const auto rc = HttpDownloader::downloadToFile(t.url, t.path, onChunk, cancel, server.username, server.password);
#endif
    if (rc == HttpDownloader::OK) {
      clearBookCache(t.path);
      state.synced.push_back(t.id);
      ++result.downloaded;
      saveState(state);  // a later failure or power loss keeps what we got
      // Have the Library's Covers view ready for the new book. Best effort: a
      // coverless book or a tight heap only means the grid generates the thumb
      // (or shows its placeholder) later, never a failed sync.
      if (!(cancel && *cancel) && library_covers::heapAllowsThumb() && !library_covers::generateThumb(t.path)) {
        LOG_DBG("SYNC", "No cover thumb for %s", t.path.c_str());
      }
    } else if (rc == HttpDownloader::ABORTED) {
      break;
    } else {
      LOG_ERR("SYNC", "Download failed (%d): %s", static_cast<int>(rc), t.url.c_str());
      ++result.failed;
    }
  }

  if (result.downloaded > 0) library::markLibraryIndexDirty();
  const bool cancelled = cancel && *cancel;
  result.ok = result.error.empty() && !cancelled;
  if (result.ok) {
    const time_t now = nowEpoch();
    if (now) state.lastSync = now;
  }
  saveState(state);
  LOG_INF("SYNC", "Done: %d new, %d already here, %d failed", result.downloaded, result.alreadyHere,
          result.failed);
  return result;
}

}  // namespace homesync
