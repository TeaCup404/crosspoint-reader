#include "KoAuto.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <PersistableStore.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "Diag.h"
#include "QuietWifi.h"

namespace homesync::koauto {
namespace {

constexpr const char* STATE_FILE = "/.crosspoint/koauto.json";
constexpr uint8_t DEFAULT_MODE = 1;
constexpr uint32_t WIFI_JOIN_MS = 8000;

struct Captured {
  bool valid = false;
  std::string path;
  std::string xpath;
  float percentage = 0;
} captured;

}  // namespace

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

void capture(const std::string& epubPath, const std::string& xpath, const float percentage) {
  captured.valid = !epubPath.empty() && !xpath.empty();
  captured.path = epubPath;
  captured.xpath = xpath;
  captured.percentage = percentage;
}

void uploadCaptured() {
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
  progress.document = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                          ? KOReaderDocumentId::calculateFromFilename(captured.path)
                          : KOReaderDocumentId::calculate(captured.path);
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
