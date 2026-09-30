#pragma once
#include <cstdint>
#include <string>

/**
 * Automatic KOReader progress upload (x4pro-homesync fork).
 *
 * When the reader goes to sleep from an open book, the position is captured
 * (before the sleep screen needs the framebuffer) and, after the sleep screen
 * is up, uploaded to the configured kosync server over a quietly joined saved
 * Wi-Fi network. No known network in range: skipped, the manual "Sync
 * progress" still works. Upload only: it never moves the local position.
 *
 * Mode lives in /.crosspoint/koauto.json: 0 = off, 1 = upload on sleep
 * (default).
 */
namespace homesync::koauto {

uint8_t mode();
bool setMode(uint8_t mode);

// True when a capture is worth doing (mode on, kosync login configured).
bool wanted();

// Called by the reader while its book is still loaded.
void capture(const std::string& epubPath, const std::string& xpath, float percentage);

// Called from the sleep path after the sleep screen is painted and before the
// radio/SD are shut down. Bounded (~15 s worst case); never throws the sleep.
void uploadCaptured();

}  // namespace homesync::koauto
