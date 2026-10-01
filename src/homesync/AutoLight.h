#pragma once
#include <ctime>

/**
 * Sun-scheduled front light (x4pro-homesync fork).
 *
 * Day (sunrise + 20 min .. sunset - 20 min): light off. Dark: light on, warm
 * and dim. Sunrise/sunset are computed on the device from the date (RTC, UTC)
 * and a fixed location. Brightness/warmth changed by the user while it is dark
 * become the dark levels; turning the light on or off by hand holds until the
 * next sunrise/sunset. Levels and the hold live in /.crosspoint/autolight.json.
 * Enabled by SETTINGS.autoLight.
 */
namespace homesync::autolight {

// After Frontlight.begin(). A silent restart keeps the live light as it was.
void begin(bool silentReboot);

// From the main loop; does real work every 30 s.
void tick();

// For the serial status line: true when it is light outside at `t` (UTC).
bool isDay(time_t t);

}  // namespace homesync::autolight
