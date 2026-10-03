#pragma once
#include <cstdint>

/**
 * Nightly firmware update (x4pro-homesync fork).
 *
 * Every deep sleep arms the RTC timer for the next 03:30 local time. That
 * wake never touches the display or the light: it joins a saved Wi-Fi quietly,
 * checks reader-hub for a newer firmware, installs it, and sleeps again (the
 * reboot after an install is routed straight back to sleep too). Skipped on a
 * low battery unless on USB power.
 */
namespace homesync::nightly {

// This boot is the timer wake, or the reboot right after a nightly install.
bool isNightlyBoot();

// The nightly job. Returns when done (no update, failure, or skipped); an
// installed update reboots from here.
void run(uint16_t batteryPercent, bool usbPower);

// Before deep sleep: arm the RTC timer for the next 03:30 (no-op when the
// clock was never set).
void armTimer();

}  // namespace homesync::nightly
