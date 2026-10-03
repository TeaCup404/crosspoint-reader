#include "RecapActivity.h"

#if HOMESYNC

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"
#include "homesync/WifiLinger.h"

namespace fui = freeink::ui;

namespace {
// reader-hub's LAN address; HttpDownloader routes it through Tailscale when away.
constexpr const char* RECAP_URL = "http://192.168.1.124:8790/recap";
constexpr unsigned long POLL_MS = 6000;
constexpr size_t MAX_BODY = 8192;

std::string urlEncode(const std::string& s) {
  std::string out;
  for (const unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}
}  // namespace

RecapActivity::RecapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                             std::string title, std::string author, const float percentage)
    : Activity("Recap", renderer, mappedInput),
      UiAppHost(renderer),
      epubPath(std::move(epubPath)),
      title(std::move(title)),
      author(std::move(author)),
      percentage(percentage) {}

void RecapActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.setScreen(&RecapActivity::rootScreen, this);
  char pct[8];
  snprintf(pct, sizeof(pct), "%.0f%%", percentage * 100.0f);
  heading = title + " - " + pct;
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::LOADING;
    body = "Asking homebot...";
    requestUpdate();
    return;
  }
  state = State::WIFI;
  body = "Connecting to Wi-Fi...";
  wifiUsed = true;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = State::FAILED;
                             body = "No Wi-Fi connection";
                           } else {
                             state = State::LOADING;
                             body = "Asking homebot...";
                           }
                           requestUpdate();
                         });
}

void RecapActivity::onExit() {
  Activity::onExit();
  if (wifiUsed || WiFi.getMode() != WIFI_MODE_NULL) {
    if (keepWifiAfterUse()) return;
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void RecapActivity::backToBook() {
  if (epubPath.empty()) {
    onGoHome();  // opened by the CMD:RECAP test hook, not from a book
    return;
  }
  activityManager.goToReader(epubPath);
}

void RecapActivity::fetch() {
  wifiUsed = true;
  char pct[16];
  snprintf(pct, sizeof(pct), "%.4f", percentage);
  const std::string url = std::string(RECAP_URL) + "?title=" + urlEncode(title) + "&author=" + urlEncode(author) +
                          "&pct=" + pct;
  std::string reply;
  const bool ok = HttpDownloader::fetchUrl(url, [&reply](const uint8_t* data, size_t len) {
    if (reply.size() + len > MAX_BODY) return false;
    reply.append(reinterpret_cast<const char*>(data), len);
    return true;
  });
  JsonDocument doc;
  if (!ok || deserializeJson(doc, reply)) {
    state = State::FAILED;
    body = ok ? "Unexpected reply from homebot" : "Homebot not reachable";
    requestUpdate(true);
    return;
  }
  const std::string status = doc["status"] | "";
  if (status == "ready") {
    state = State::SHOWN;
    body = doc["recap"] | "";
  } else if (status == "preparing") {
    state = State::WAITING;
    const int done = doc["done"] | 0;
    const int total = doc["total"] | 0;
    body = "Homebot is reading the book so far (" + std::to_string(done) + " of " + std::to_string(total) +
           "). The first recap of a book takes a few minutes; later ones are quick.";
    nextPollAt = millis() + POLL_MS;
  } else {
    state = State::FAILED;
    body = doc["message"] | "Recap failed";
  }
  LOG_INF("RECAP", "%s: %s", title.c_str(), status.c_str());
  requestUpdate(true);
}

void RecapActivity::loop() {
  if (state == State::WIFI) return;
  if (state == State::LOADING) {
    fetch();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    backToBook();
    return;
  }
  int tx = 0;
  int ty = 0;
  const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty);
  if (state == State::SHOWN || state == State::FAILED) {
    if (confirm) backToBook();
    return;
  }
  if (state == State::WAITING && (confirm || millis() >= nextPollAt)) {
    fetch();
  }
}

void RecapActivity::rootScreen(UiScreen& screen, void* user) { static_cast<RecapActivity*>(user)->draw(screen); }

void RecapActivity::draw(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  auto& target = screen.target();

  fui::HeaderProps header;
  header.title = "Story so far";
  header.borderEdges = fui::EdgeBottom;
  GUI.applyHeaderStatus(renderer, header);
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  const auto frameRect = screen.frame().screen();
  fui::header(screen.frame(),
              fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding), frameRect.width,
                        static_cast<int16_t>(metrics.headerHeight)},
              header);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing),
                  theme.spaceLg, static_cast<int16_t>(metrics.buttonHintsHeight), theme.spaceLg});

  fui::TextStyle small = theme.bodyText;
  small.font = theme.fontSmall;
  small.bold = true;
  const int16_t sh = target.lineHeight(small.font);
  target.text(screen.takeTop(sh, theme.spaceMd), heading.c_str(), small);

  fui::TextStyle text = theme.bodyText;
  if (state != State::SHOWN) text.align = fui::TextAlign::Center;
  fui::Rect area = screen.body();
  const int16_t lh = target.lineHeight(text.font);
  // The fonts have no newline glyph: lay out each paragraph on its own, with
  // a gap between them, until the screen is full.
  size_t start = 0;
  while (start < body.size() && area.height >= lh) {
    size_t end = body.find('\n', start);
    if (end == std::string::npos) end = body.size();
    const std::string para = body.substr(start, end - start);
    start = end + 1;
    if (para.find_first_not_of(" \t\r") == std::string::npos) continue;
    text.maxLines = static_cast<uint8_t>(std::min(255, area.height / lh));
    const int16_t h = fui::measureWrappedText(target, para.c_str(), text, area.width).height;
    target.text(fui::Rect{area.x, area.y, area.width, h}, para.c_str(), text);
    const int16_t used = static_cast<int16_t>(h + theme.spaceMd);
    area.y = static_cast<int16_t>(area.y + used);
    area.height = static_cast<int16_t>(std::max(0, area.height - used));
  }
}

void RecapActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const char* confirm = state == State::WAITING ? "Check now" : (state == State::SHOWN ? "Back to book" : "");
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderUi();
  renderer.displayBuffer();
}

#endif  // HOMESYNC
