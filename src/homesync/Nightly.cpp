#include "Nightly.h"

#if HOMESYNC
#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "Diag.h"
#include "QuietWifi.h"
#include "Tailnet.h"
#include "network/OtaUpdater.h"

namespace homesync::nightly {
namespace {

constexpr int RUN_HOUR = 3;
constexpr int RUN_MINUTE = 30;
constexpr uint16_t MIN_BATTERY = 25;
constexpr uint32_t WIFI_JOIN_MS = 10000;
constexpr uint32_t INSTALL_REBOOT_MAGIC = 0x4E495445;  // "NITE"

// Survives the esp_restart() after an install (not power loss).
RTC_NOINIT_ATTR uint32_t installRebootMagic;

void wifiOff() {
  tailnet::down();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

}  // namespace

bool isNightlyBoot() {
  if (installRebootMagic == INSTALL_REBOOT_MAGIC) return true;
  return esp_reset_reason() == ESP_RST_DEEPSLEEP && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

void run(const uint16_t batteryPercent, const bool usbPower) {
  if (installRebootMagic == INSTALL_REBOOT_MAGIC) {
    installRebootMagic = 0;
    LOG_INF("NIGHT", "Booted the update installed tonight (" CROSSPOINT_VERSION "), back to sleep");
    return;
  }
  if (batteryPercent < MIN_BATTERY && !usbPower) {
    LOG_INF("NIGHT", "Battery %u%%, skipping tonight", batteryPercent);
    return;
  }
  if (!quietConnect(WIFI_JOIN_MS)) {
    LOG_INF("NIGHT", "No known Wi-Fi, skipping tonight");
    wifiOff();
    return;
  }
  if (diag::pending()) diag::flush();

  OtaUpdater ota;
  const auto rc = ota.checkForUpdate();
  if (rc != OtaUpdater::OK || !ota.isUpdateNewer()) {
    LOG_INF("NIGHT", "No newer firmware (rc=%d, latest %s)", static_cast<int>(rc), ota.getLatestVersion().c_str());
    wifiOff();
    return;
  }
  LOG_INF("NIGHT", "Installing %s", ota.getLatestVersion().c_str());
  const auto irc = ota.installUpdate();
  if (irc != OtaUpdater::OK) {
    LOG_ERR("NIGHT", "Install failed: %d", static_cast<int>(irc));
    diag::record("nightly", "update install failed");
    wifiOff();
    return;
  }
  wifiOff();
  installRebootMagic = INSTALL_REBOOT_MAGIC;
  esp_restart();
}

void armTimer() {
  struct tm now{};
  if (!halClock.localTime(now) || now.tm_year < 120) return;  // clock never set
  int64_t secs = static_cast<int64_t>(RUN_HOUR * 3600 + RUN_MINUTE * 60) -
                 static_cast<int64_t>(now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec);
  if (secs < 60) secs += 24 * 3600;  // tonight's run has passed (or is under a minute away)
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(secs) * 1000000ULL);
  LOG_DBG("NIGHT", "Timer armed: %lld s", static_cast<long long>(secs));
}

}  // namespace homesync::nightly
#endif  // HOMESYNC
