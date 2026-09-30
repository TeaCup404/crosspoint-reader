#include "QuietWifi.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include "WifiCredentialStore.h"

namespace homesync {

bool quietConnect(const uint32_t perNetworkMs) {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) return true;
  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentialCount() == 0) return false;
  WiFi.mode(WIFI_STA);
  const int found = WiFi.scanNetworks();
  for (int i = 0; i < found; ++i) {
    const auto cred = WIFI_STORE.findCredential(WiFi.SSID(i).c_str());
    if (!cred) continue;
    LOG_INF("WIFI", "Quiet join: %s", cred->ssid.c_str());
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    const uint32_t deadline = millis() + perNetworkMs;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.scanDelete();
      return true;
    }
    WiFi.disconnect(false);
  }
  WiFi.scanDelete();
  return false;
}

}  // namespace homesync
