#pragma once
#include <cstdint>

namespace homesync {

// Joins a saved Wi-Fi network without any UI: scan, then try each saved SSID
// that is in range for up to `perNetworkMs`. Returns false (and leaves the
// radio on) when none connects. Used by automatic features that must never
// show the network picker.
bool quietConnect(uint32_t perNetworkMs = 15000);

}  // namespace homesync
