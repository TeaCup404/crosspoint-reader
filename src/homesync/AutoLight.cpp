#include "AutoLight.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalFrontlight.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <cmath>

#include "CrossPointSettings.h"
#include "HomeSync.h"

namespace homesync::autolight {
namespace {

constexpr const char* STATE_FILE = "/.crosspoint/autolight.json";
// Central Israel (the reader's Jerusalem timezone); a few minutes off anywhere
// in the country.
constexpr double LATITUDE = 32.08;
constexpr double LONGITUDE = 34.78;
constexpr time_t TWILIGHT_S = 20 * 60;  // day starts after sunrise / ends before sunset by this
constexpr uint32_t TICK_MS = 30000;
constexpr uint8_t DEFAULT_DARK_BRIGHTNESS = 30;
constexpr uint8_t DEFAULT_DARK_WARMTH = 80;

struct State {
  uint8_t darkBrightness = DEFAULT_DARK_BRIGHTNESS;
  uint8_t darkWarmth = DEFAULT_DARK_WARMTH;
  time_t holdUntil = 0;  // manual on/off wins until this sunrise/sunset
} state;

bool applied = false;  // the live light is what apply() last set
bool appliedDay = false;
bool appliedOn = false;
uint8_t appliedBrightness = 0;
uint8_t appliedWarmth = 0;
uint32_t lastTick = 0;
bool loaded = false;

void load() {
  loaded = true;
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(STATE_FILE, doc)) return;
  state.darkBrightness = doc["darkBrightness"] | DEFAULT_DARK_BRIGHTNESS;
  state.darkWarmth = doc["darkWarmth"] | DEFAULT_DARK_WARMTH;
  state.holdUntil = doc["holdUntil"] | static_cast<time_t>(0);
}

void save() {
  JsonDocument doc;
  doc["darkBrightness"] = state.darkBrightness;
  doc["darkWarmth"] = state.darkWarmth;
  doc["holdUntil"] = state.holdUntil;
  PersistableStoreBase::writeDocToFile(STATE_FILE, doc);
}

// Sunrise equation (NOAA approximation, ~1 min) for the solar day `n` days
// after J2000; returns UTC epochs.
void sunTimes(const long n, time_t& rise, time_t& set) {
  constexpr double DEG = M_PI / 180.0;
  const double jStar = n - LONGITUDE / 360.0;
  const double m = std::fmod(357.5291 + 0.98560028 * jStar, 360.0);
  const double c = 1.9148 * std::sin(m * DEG) + 0.02 * std::sin(2 * m * DEG) + 0.0003 * std::sin(3 * m * DEG);
  const double lambda = std::fmod(m + c + 180.0 + 102.9372, 360.0);
  const double transit = 2451545.0 + jStar + 0.0053 * std::sin(m * DEG) - 0.0069 * std::sin(2 * lambda * DEG);
  const double sinDecl = std::sin(lambda * DEG) * std::sin(23.4397 * DEG);
  const double cosDecl = std::cos(std::asin(sinDecl));
  const double cosHour =
      (std::sin(-0.833 * DEG) - std::sin(LATITUDE * DEG) * sinDecl) / (std::cos(LATITUDE * DEG) * cosDecl);
  const double hour = std::acos(std::max(-1.0, std::min(1.0, cosHour))) / DEG;
  rise = static_cast<time_t>((transit - hour / 360.0 - 2440587.5) * 86400.0);
  set = static_cast<time_t>((transit + hour / 360.0 - 2440587.5) * 86400.0);
}

long solarDay(const time_t t) { return std::lround(t / 86400.0 + 2440587.5 - 2451545.0); }

// Next day/dark boundary after t.
time_t nextBoundary(const time_t t) {
  time_t best = t + 86400;
  for (long n = solarDay(t) - 1; n <= solarDay(t) + 1; ++n) {
    time_t rise, set;
    sunTimes(n, rise, set);
    for (const time_t b : {rise + TWILIGHT_S, set - TWILIGHT_S}) {
      if (b > t && b < best) best = b;
    }
  }
  return best;
}

void apply(const bool day) {
  if (day) {
    Frontlight.setOn(false);
  } else {
    Frontlight.setBrightness(state.darkBrightness);
    Frontlight.setWarmth(state.darkWarmth);
    Frontlight.setOn(true);
  }
  SETTINGS.frontlightOn = day ? 0 : 1;
  applied = true;
  appliedDay = day;
  appliedOn = Frontlight.isOn();
  appliedBrightness = Frontlight.brightness();
  appliedWarmth = Frontlight.warmth();
  LOG_INF("ALIGHT", "%s: light %s", day ? "Day" : "Dark", day ? "off" : "on");
}

bool enabled() { return SETTINGS.autoLight != 0 && Frontlight.present(); }

void update(const bool atWake) {
  const time_t now = nowEpoch();
  if (now == 0) return;  // clock never set: leave the light alone

  if (applied && !atWake) {
    if (Frontlight.isOn() != appliedOn) {
      // Switched by hand: hold that until the next sunrise/sunset.
      state.holdUntil = nextBoundary(now);
      applied = false;
      save();
      LOG_INF("ALIGHT", "Manual %s, held until next sun change", Frontlight.isOn() ? "on" : "off");
      return;
    }
    if (!appliedDay && appliedOn &&
        (Frontlight.brightness() != appliedBrightness || Frontlight.warmth() != appliedWarmth)) {
      // Adjusted in the dark: these become the dark levels.
      appliedBrightness = state.darkBrightness = Frontlight.brightness();
      appliedWarmth = state.darkWarmth = Frontlight.warmth();
      save();
      LOG_INF("ALIGHT", "Dark levels now %u%% / warm %u%%", state.darkBrightness, state.darkWarmth);
    }
  }
  if (now < state.holdUntil) return;
  const bool day = isDay(now);
  if (!applied || day != appliedDay) apply(day);
}

}  // namespace

bool isDay(const time_t t) {
  for (long n = solarDay(t) - 1; n <= solarDay(t) + 1; ++n) {
    time_t rise, set;
    sunTimes(n, rise, set);
    if (t >= rise + TWILIGHT_S && t < set - TWILIGHT_S) return true;
  }
  return false;
}

void begin(const bool silentReboot) {
  if (!enabled()) return;
  load();
  lastTick = millis();
  if (silentReboot) return;  // keep the light exactly as it was; tick() resumes the schedule
  update(true);
}

void tick() {
  if (millis() - lastTick < TICK_MS || !enabled()) return;
  lastTick = millis();
  if (!loaded) load();  // switched on in Settings after boot
  update(false);
}

}  // namespace homesync::autolight
