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
 * Before a book's first upload, its document id is sent to reader-hub
 * (/kolink), which records it in Calibre-Web's checksum table so CWA can tie
 * the progress to the library book (read status, progress on the book page).
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
void capture(const std::string& epubPath, const std::string& xpath, float percentage, const std::string& title,
             const std::string& author);

// Called from the sleep path after the sleep screen is painted and before the
// radio/SD are shut down. Bounded: ~15 s at home, up to ~60 s more when away
// (Tailscale join); never throws the sleep.
void uploadCaptured();

// ---- pull on open ----
// When a book opens, a background task quietly joins Wi-Fi and fetches the
// server position; if another device (KOReader on a phone) is further ahead,
// the reader runs the KOReader sync in automatic mode to jump there. At most
// once per book per boot, and never on the silent restart back into the book.
enum class PullResult : uint8_t { None, Pending, RemoteAhead };
void skipPullOnce();  // the next startPull() is ignored (silent restart into the reader)
void startPull(const std::string& epubPath, float localPercentage);
PullResult pullResult();
void clearPull();
// Asks a running pull to stop early (it then turns Wi-Fi off as usual).
void cancelPull();
// Waits (bounded) for a running pull before something else uses Wi-Fi.
void waitForPull(uint32_t maxMs);

// KOReaderSyncClient::transport for this build: home-server URLs go over
// Tailscale when the reader is away from the home LAN.
bool tunnelTransport(const char* method, const std::string& url, const std::string& headers,
                     const std::string& body, int& status, std::string& response);

}  // namespace homesync::koauto
