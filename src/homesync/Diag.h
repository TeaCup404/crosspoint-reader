#pragma once
#include <string>

/**
 * Remote diagnostics (x4pro-homesync fork): failures the user sees on the
 * device are saved to the SD card with the recent log lines, and sent to
 * reader-hub (/diag) the next time the reader is online, so problems can be
 * debugged without a USB cable. Also sends /crash_report.txt once.
 */
namespace homesync::diag {

// Save a report: what happened + the last log lines (Logging's RTC ring).
void record(const char* what, const std::string& detail);

// True when there is something to send.
bool pending();

// Send everything pending to reader-hub (Wi-Fi must be up; routes over
// Tailscale when away). Sent reports are deleted.
void flush();

// Percent-encodes everything but unreserved characters (query values).
std::string urlEncode(const std::string& s);

}  // namespace homesync::diag
