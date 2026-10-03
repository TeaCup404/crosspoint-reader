#include "WifiLinger.h"

#if HOMESYNC
#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include "Tailnet.h"

namespace {
constexpr uint32_t LINGER_MS = 5UL * 60UL * 1000UL;
volatile bool lingering = false;
volatile uint32_t releasedAt = 0;
}  // namespace

bool keepWifiAfterUse() {
  if (WiFi.status() != WL_CONNECTED) return false;
  releasedAt = millis();
  lingering = true;
  LOG_INF("WIFI", "Staying connected for 5 min (heap %u, max block %u)", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  return true;
}

void wifiLingerTick(const bool onPlainScreen) {
  if (!lingering) return;
  if (WiFi.status() != WL_CONNECTED) {
    lingering = false;
    return;
  }
  // A network screen reopened during the linger owns the link; its exit
  // restarts the timer.
  if (!onPlainScreen) {
    releasedAt = millis();
    return;
  }
  if (millis() - releasedAt < LINGER_MS) return;
  lingering = false;
  homesync::tailnet::down();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG_INF("WIFI", "Linger over, Wi-Fi off (heap %u, max block %u)", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}
#else
bool keepWifiAfterUse() { return false; }
void wifiLingerTick(bool) {}
#endif
