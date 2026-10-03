#pragma once

/**
 * Wi-Fi linger (x4pro-homesync fork): a network screen that is done with
 * Wi-Fi leaves it connected for 5 minutes instead of disconnecting and
 * silent-rebooting, so the next sync / Server / OPDS / update / recap reuses
 * the link without a scan and join. The link is dropped once the reader has
 * sat on a plain screen (Home, a book, Library, Files, Settings) for that
 * long, and always at sleep. The PSRAM board keeps the Wi-Fi/TLS heap churn
 * out of the way the silent reboot exists for on the C3.
 */

// Called where a network screen would tear Wi-Fi down. True = still connected
// and now lingering: skip the disconnect and silent reboot. Always false in
// builds without HOMESYNC.
bool keepWifiAfterUse();

// Main loop: drops the lingering link after 5 minutes on a plain screen.
void wifiLingerTick(bool onPlainScreen);
