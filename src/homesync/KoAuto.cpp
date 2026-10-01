#include "KoAuto.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "Diag.h"
#include "QuietWifi.h"
#include "Tailnet.h"
#include "network/HttpDownloader.h"

namespace homesync::koauto {
namespace {

constexpr const char* STATE_FILE = "/.crosspoint/koauto.json";
constexpr uint8_t DEFAULT_MODE = 1;
constexpr uint32_t WIFI_JOIN_MS = 8000;
constexpr const char* LINKED_FILE = "/.crosspoint/kolinked.json";  // document ids CWA already knows
constexpr const char* LINK_URL = "http://192.168.1.124:8790/kolink";  // reader-hub; via Tailscale when away
constexpr size_t MAX_LINKED = 64;

struct Captured {
  bool valid = false;
  std::string path;
  std::string xpath;
  float percentage = 0;
  std::string title;
  std::string author;
} captured;

// Tells reader-hub which library book a document id belongs to, once per id.
void linkOnce(const std::string& document, const std::string& title, const std::string& author) {
  JsonDocument doc;
  PersistableStoreBase::readDocFromFile(LINKED_FILE, doc);
  JsonArray ids = doc["ids"].is<JsonArray>() ? doc["ids"].as<JsonArray>() : doc["ids"].to<JsonArray>();
  for (const JsonVariant id : ids) {
    if (document == (id | "")) return;
  }
  const std::string url = std::string(LINK_URL) + "?doc=" + diag::urlEncode(document) +
                          "&title=" + diag::urlEncode(title) + "&author=" + diag::urlEncode(author);
  std::string reply;
  if (!HttpDownloader::fetchUrl(url, [&reply](const uint8_t* d, size_t n) {
        reply.append(reinterpret_cast<const char*>(d), n);
        return reply.size() < 512;
      })) {
    LOG_ERR("KOAUTO", "Link request failed");
    return;
  }
  JsonDocument answer;
  if (deserializeJson(answer, reply) || !(answer["linked"] | false)) {
    LOG_INF("KOAUTO", "Not linked: %s", reply.c_str());
    return;
  }
  while (ids.size() >= MAX_LINKED) ids.remove(0);
  ids.add(document);
  PersistableStoreBase::writeDocToFile(LINKED_FILE, doc);
  LOG_INF("KOAUTO", "Linked %s to library book %d", document.c_str(), answer["book"] | 0);
}

}  // namespace

namespace {
constexpr char OWN_DEVICE_ID[] = "crosspoint-reader";  // KOReaderSyncClient's device_id
constexpr float PULL_MARGIN = 0.002f;                   // ignore float noise between devices
constexpr uint32_t PULL_WIFI_MS = 6000;

volatile PullResult pullState = PullResult::None;
bool pullCancel = false;
TaskHandle_t pullTaskHandle = nullptr;
bool pullSkip = false;
std::string pulledPath;  // books already checked this boot
std::string pullPath;
float pullLocal = 0;

std::string documentIdFor(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(path)
             : KOReaderDocumentId::calculate(path);
}

void pullTask(void*) {
  PullResult result = PullResult::None;
  if (quietConnect(PULL_WIFI_MS) && !pullCancel) {
    KOReaderProgress remote{};
    const auto status = KOReaderSyncClient::getProgress(documentIdFor(pullPath), remote);
    if (!pullCancel && status == KOReaderSyncClient::OK && remote.deviceId != OWN_DEVICE_ID &&
        remote.percentage > pullLocal + PULL_MARGIN) {
      LOG_INF("KOAUTO", "Server is ahead: %.2f%% (%s) vs %.2f%%", remote.percentage * 100.0f, remote.device.c_str(),
              pullLocal * 100.0f);
      result = PullResult::RemoteAhead;  // Wi-Fi stays up for the sync activity
    } else {
      LOG_INF("KOAUTO", "Pull: nothing newer (%s)", KOReaderSyncClient::errorString(status));
    }
  }
  if (result != PullResult::RemoteAhead) {
    tailnet::down();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  LOG_DBG("KOAUTO", "Pull task stack left: %u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  pullTaskHandle = nullptr;
  pullState = result;
  vTaskDelete(nullptr);
}
}  // namespace

void skipPullOnce() { pullSkip = true; }

void startPull(const std::string& epubPath, const float localPercentage) {
  if (pullSkip) {
    pullSkip = false;
    pulledPath = epubPath;
    return;
  }
  if (pullState == PullResult::Pending || epubPath == pulledPath || !wanted()) return;
  pulledPath = epubPath;
  pullPath = epubPath;
  pullLocal = localPercentage;
  pullCancel = false;
  pullState = PullResult::Pending;
  // TLS (wolfSSL) and MicroLink need a deep stack; this runs once per book open.
  if (xTaskCreate(pullTask, "KoPull", 16384, nullptr, 1, &pullTaskHandle) != pdPASS) {
    LOG_ERR("KOAUTO", "Pull task not started");
    pullState = PullResult::None;
  }
}

PullResult pullResult() { return pullState; }

void clearPull() {
  if (pullState != PullResult::Pending) pullState = PullResult::None;
}

void cancelPull() {
  if (pullState == PullResult::Pending) pullCancel = true;
}

void waitForPull(const uint32_t maxMs) {
  const uint32_t start = millis();
  while (pullState == PullResult::Pending && millis() - start < maxMs) delay(50);
}

bool tunnelTransport(const char* method, const std::string& url, const std::string& headers,
                     const std::string& body, int& status, std::string& response) {
  std::string tunnelUrl;
  bool direct = false;
  if (!tailnet::tailnetUrlFor(url, tunnelUrl, direct)) return false;
  if ((!direct && tailnet::atHome()) || !tailnet::configured()) return false;
  status = 0;
  // Only the pull task may be cancelled from outside.
  const bool* cancel = xTaskGetCurrentTaskHandle() == pullTaskHandle ? &pullCancel : nullptr;
  if (!tailnet::up(tailnet::HOMEBOT_TAILNET_IP, [](const char*) {}, cancel)) return true;
  LOG_DBG("KOAUTO", "Via Tailscale: %s %s", method, tunnelUrl.c_str());
  tailnet::request(method, tunnelUrl, headers, body, status, response);
  return true;
}

uint8_t mode() {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(STATE_FILE, doc)) return DEFAULT_MODE;
  return doc["mode"] | DEFAULT_MODE;
}

bool setMode(const uint8_t m) {
  JsonDocument doc;
  doc["mode"] = m;
  return PersistableStoreBase::writeDocToFile(STATE_FILE, doc);
}

bool wanted() { return mode() != 0 && KOREADER_STORE.hasCredentials(); }

void capture(const std::string& epubPath, const std::string& xpath, const float percentage, const std::string& title,
             const std::string& author) {
  captured.valid = !epubPath.empty() && !xpath.empty();
  captured.path = epubPath;
  captured.xpath = xpath;
  captured.percentage = percentage;
  captured.title = title;
  captured.author = author;
}

void uploadCaptured() {
  cancelPull();
  waitForPull(15000);
  const bool havePosition = captured.valid;
  const bool haveDiag = diag::pending();
  if (!havePosition && !haveDiag) return;
  captured.valid = false;
  const uint32_t started = millis();
  if (!quietConnect(WIFI_JOIN_MS)) {
    LOG_INF("KOAUTO", "No known Wi-Fi in range, skipped");
    return;
  }
  if (haveDiag) diag::flush();
  if (!havePosition) return;
  KOReaderProgress progress{};
  progress.document = documentIdFor(captured.path);
  if (!captured.title.empty()) linkOnce(progress.document, captured.title, captured.author);
  progress.progress = captured.xpath;
  progress.percentage = captured.percentage;
  const auto result = KOReaderSyncClient::updateProgress(progress);
  if (result == KOReaderSyncClient::OK) {
    LOG_INF("KOAUTO", "Uploaded %.2f%% in %lu ms", captured.percentage * 100.0f,
            static_cast<unsigned long>(millis() - started));
  } else {
    LOG_ERR("KOAUTO", "Upload failed: %s", KOReaderSyncClient::errorString(result));
  }
}

}  // namespace homesync::koauto
