#pragma once
#include <functional>
#include <string>

#include "OpdsServerStore.h"

/**
 * Home library sync (x4pro-homesync fork).
 *
 * Pulls every EPUB on one OPDS feed that is not on the device yet. With no
 * feed configured, it looks for a Calibre-Web shelf titled "To Reader" and
 * falls back to the newest books (capped). IDs of synced books are remembered,
 * so a book deleted on the device is not downloaded again.
 *
 * State and options live in /.crosspoint/homesync.json:
 *   { "shelf": "To Reader",   // shelf title to look for (case-insensitive)
 *     "feed": "",             // explicit feed path/URL, overrides "shelf"
 *     "maxNew": 10,           // cap for the newest-books fallback
 *     "autoHours": 6,         // auto-sync on wake-to-home if older; 0 = off
 *     "lastSync": 0,          // epoch of the last successful sync
 *     "lastAttempt": 0,       // epoch of the last automatic attempt
 *     "synced": ["urn:..."] } // OPDS entry ids already pulled
 */
namespace homesync {

// Pseudo-href for the "Sync library" row the OPDS browser prepends.
constexpr const char* SYNC_ROW_HREF = "homesync:sync";

struct Progress {
  int index = 0;  // 1-based book being downloaded
  int total = 0;  // books to download this run
  const char* title = "";
  size_t bytes = 0;
  size_t totalBytes = 0;
};

struct Result {
  bool ok = false;
  int downloaded = 0;
  int alreadyHere = 0;
  int failed = 0;
  std::string source;  // which feed was synced (for the summary screen)
  std::string error;
};

using ProgressFn = std::function<void(const Progress&)>;

// True when auto-sync is on and both the last sync and the last auto attempt
// are older than autoHours.
bool autoSyncDue();

// Records that an automatic sync was started, so a failing one (e.g. away from
// any known Wi-Fi) is not retried on every wake.
void noteAutoAttempt();

// Runs one sync against `server`. Wi-Fi must already be connected. `status`
// is called with short phase messages, `progress` per download chunk.
Result run(const OpdsServer& server, const std::function<void(const char*)>& status, const ProgressFn& progress,
           bool* cancel);

}  // namespace homesync
