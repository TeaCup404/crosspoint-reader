#include "Diag.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

#include "network/HttpDownloader.h"

namespace homesync::diag {
namespace {

constexpr const char* DIAG_FILE = "/.crosspoint/diag.txt";
constexpr const char* CRASH_FILE = "/crash_report.txt";
constexpr const char* CRASH_SENT = "/.crosspoint/crash_sent";  // size of the last crash report sent
constexpr const char* DIAG_URL = "http://192.168.1.124:8790/diag";  // reader-hub; routed via Tailscale when away
constexpr size_t MAX_FILE = 6000;
constexpr size_t PIECE = 1200;  // characters per request (URL-encoded, so up to ~3.6 KB)

std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string readAll(const char* path, size_t limit) {
  std::string out;
  HalFile f;
  if (!Storage.openFileForRead("DIAG", path, f)) return out;
  char buf[256];
  while (out.size() < limit) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  f.close();
  if (out.size() > limit) out.resize(limit);
  return out;
}

bool send(const char* kind, const std::string& text) {
  for (size_t off = 0, part = 0; off < text.size(); off += PIECE, ++part) {
    const std::string url = std::string(DIAG_URL) + "?kind=" + kind + "&part=" + std::to_string(part) +
                            "&version=" + urlEncode(CROSSPOINT_VERSION) +
                            "&text=" + urlEncode(text.substr(off, PIECE));
    std::string reply;
    if (!HttpDownloader::fetchUrl(url, [&reply](const uint8_t* d, size_t n) {
          reply.append(reinterpret_cast<const char*>(d), n);
          return reply.size() < 256;
        })) {
      return false;
    }
  }
  return true;
}

size_t crashSize() {
  HalFile f;
  if (!Storage.openFileForRead("DIAG", CRASH_FILE, f)) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

size_t crashSentSize() { return static_cast<size_t>(atol(readAll(CRASH_SENT, 32).c_str())); }

}  // namespace

void record(const char* what, const std::string& detail) {
  std::string entry = "=== ";
  entry += what;
  entry += " (" CROSSPOINT_VERSION ")\n";
  entry += detail;
  entry += "\n--- recent log ---\n";
  entry += getLastLogs();
  entry += "\n";
  std::string existing = readAll(DIAG_FILE, MAX_FILE);
  if (existing.size() + entry.size() > MAX_FILE) existing.clear();  // keep the newest
  HalFile f;
  if (!Storage.openFileForWrite("DIAG", DIAG_FILE, f)) return;
  f.write(existing.data(), existing.size());
  f.write(entry.data(), entry.size());
  f.close();
  LOG_INF("DIAG", "Recorded: %s", what);
}

bool pending() {
  if (Storage.exists(DIAG_FILE)) return true;
  const size_t crash = crashSize();
  return crash > 0 && crash != crashSentSize();
}

void flush() {
  if (Storage.exists(DIAG_FILE)) {
    const std::string text = readAll(DIAG_FILE, MAX_FILE);
    if (!text.empty() && send("diag", text)) {
      Storage.remove(DIAG_FILE);
      LOG_INF("DIAG", "Sent %u bytes", static_cast<unsigned>(text.size()));
    }
  }
  const size_t crash = crashSize();
  if (crash > 0 && crash != crashSentSize()) {
    if (send("crash", readAll(CRASH_FILE, MAX_FILE))) {
      HalFile f;
      if (Storage.openFileForWrite("DIAG", CRASH_SENT, f)) {
        const std::string n = std::to_string(crash);
        f.write(n.data(), n.size());
        f.close();
      }
      LOG_INF("DIAG", "Sent crash report");
    }
  }
}

}  // namespace homesync::diag
