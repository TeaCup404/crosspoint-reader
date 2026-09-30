#include "Tailnet.h"

#if HOMESYNC

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <ObfuscationUtils.h>
#include <PersistableStore.h>
#include <Logging.h>
#include <mbedtls/base64.h>
#include <microlink.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "LetsEncryptRoots.h"

namespace homesync::tailnet {
namespace {

constexpr uint32_t JOIN_TIMEOUT_MS = 60000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t IO_TIMEOUT_MS = 20000;
constexpr int MAX_REDIRECTS = 5;
constexpr const char* STATE_FILE = "/.crosspoint/tailnet.json";

struct KeyState {
  std::string key;
  bool joined = false;
};

KeyState loadKeyState() {
  KeyState s;
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(STATE_FILE, doc)) return s;
  const char* obf = doc["key_obf"] | "";
  if (*obf) s.key = obfuscation::deobfuscateFromBase64(obf);
  s.joined = doc["joined"] | false;
  return s;
}

bool saveKeyState(const KeyState& s) {
  JsonDocument doc;
  if (!s.key.empty()) doc["key_obf"] = obfuscation::obfuscateToBase64(s.key);
  doc["joined"] = s.joined;
  return PersistableStoreBase::writeDocToFile(STATE_FILE, doc);
}

microlink_t* ml = nullptr;
uint32_t serverIp = 0;

// ---- one TCP (+ optional TLS) connection over the tunnel ----

int tunnelSend(WOLFSSL*, char* buf, int sz, void* ctx) {
  auto* sock = static_cast<microlink_tcp_socket_t*>(ctx);
  return microlink_tcp_send(sock, buf, sz) == ESP_OK ? sz : WOLFSSL_CBIO_ERR_CONN_CLOSE;
}

int tunnelRecv(WOLFSSL*, char* buf, int sz, void* ctx) {
  auto* sock = static_cast<microlink_tcp_socket_t*>(ctx);
  const int n = microlink_tcp_recv(sock, buf, sz, 100);
  if (n > 0) return n;
  return n == 0 ? WOLFSSL_CBIO_ERR_WANT_READ : WOLFSSL_CBIO_ERR_CONN_CLOSE;
}

class Conn {
 public:
  ~Conn() { close(); }

  bool open(const std::string& host, uint16_t port, bool tls) {
    sock = microlink_tcp_connect(ml, serverIp, port, CONNECT_TIMEOUT_MS);
    if (!sock) {
      LOG_ERR("TSN", "TCP connect to port %u failed", port);
      return false;
    }
    if (!tls) return true;

    ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!ctx) return false;
    if (wolfSSL_CTX_load_verify_buffer(ctx, reinterpret_cast<const unsigned char*>(HOMESYNC_LE_ROOTS),
                                       strlen(HOMESYNC_LE_ROOTS), WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
      LOG_ERR("TSN", "Loading CA roots failed");
      return false;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, nullptr);
    wolfSSL_SetIORecv(ctx, tunnelRecv);
    wolfSSL_SetIOSend(ctx, tunnelSend);
    ssl = wolfSSL_new(ctx);
    if (!ssl) return false;
    wolfSSL_SetIOReadCtx(ssl, sock);
    wolfSSL_SetIOWriteCtx(ssl, sock);
    wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host.c_str(), host.size());
    wolfSSL_check_domain_name(ssl, host.c_str());
#if defined(WOLFSSL_TLS13) && defined(HAVE_CURVE25519)
    wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519);  // same heap reasoning as SecureClient
#endif
#ifdef HAVE_MAX_FRAGMENT
    wolfSSL_UseMaxFragment(ssl, WOLFSSL_MFL_2_11);
#endif
    const uint32_t deadline = millis() + IO_TIMEOUT_MS;
    int ret;
    while ((ret = wolfSSL_connect(ssl)) != WOLFSSL_SUCCESS) {
      const int err = wolfSSL_get_error(ssl, ret);
      if ((err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) || millis() > deadline) {
        LOG_ERR("TSN", "TLS handshake with %s failed: %d", host.c_str(), err);
        return false;
      }
    }
    return true;
  }

  bool writeAll(const char* data, size_t len) {
    if (!ssl) return microlink_tcp_send(sock, data, len) == ESP_OK;
    while (len > 0) {
      const int n = wolfSSL_write(ssl, data, static_cast<int>(len));
      if (n <= 0) {
        const int err = wolfSSL_get_error(ssl, n);
        if (err == WOLFSSL_ERROR_WANT_WRITE || err == WOLFSSL_ERROR_WANT_READ) continue;
        return false;
      }
      data += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }

  // >0 bytes, 0 = peer closed, -1 = error/timeout.
  int read(uint8_t* buf, size_t len) {
    const uint32_t deadline = millis() + IO_TIMEOUT_MS;
    while (millis() < deadline) {
      if (!ssl) {
        const int n = microlink_tcp_recv(sock, buf, len, 500);
        if (n != 0) return n < 0 ? 0 : n;  // MicroLink reports close as -1
        continue;
      }
      const int n = wolfSSL_read(ssl, buf, static_cast<int>(len));
      if (n > 0) return n;
      const int err = wolfSSL_get_error(ssl, n);
      if (err == WOLFSSL_ERROR_ZERO_RETURN || err == SOCKET_PEER_CLOSED_E) return 0;
      if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) return -1;
    }
    return -1;
  }

  void close() {
    if (ssl) {
      wolfSSL_free(ssl);
      ssl = nullptr;
    }
    if (ctx) {
      wolfSSL_CTX_free(ctx);
      ctx = nullptr;
    }
    if (sock) {
      microlink_tcp_close(sock);
      sock = nullptr;
    }
  }

 private:
  microlink_tcp_socket_t* sock = nullptr;
  WOLFSSL_CTX* ctx = nullptr;
  WOLFSSL* ssl = nullptr;
};

// ---- minimal HTTP/1.1 client ----

struct Url {
  bool tls = false;
  std::string host;
  uint16_t port = 0;
  std::string path;
};

bool parseUrl(const std::string& url, Url& out) {
  size_t rest;
  if (url.rfind("https://", 0) == 0) {
    out.tls = true;
    out.port = 443;
    rest = 8;
  } else if (url.rfind("http://", 0) == 0) {
    out.port = 80;
    rest = 7;
  } else {
    return false;
  }
  const size_t slash = url.find('/', rest);
  std::string hostPort = url.substr(rest, slash == std::string::npos ? std::string::npos : slash - rest);
  out.path = slash == std::string::npos ? "/" : url.substr(slash);
  const size_t colon = hostPort.find(':');
  if (colon != std::string::npos) {
    out.port = static_cast<uint16_t>(atoi(hostPort.c_str() + colon + 1));
    hostPort.resize(colon);
  }
  out.host = hostPort;
  return !out.host.empty() && out.port != 0;
}

std::string basicAuth(const std::string& user, const std::string& password) {
  const std::string plain = user + ":" + password;
  size_t outLen = 0;
  std::string out(4 * ((plain.size() + 2) / 3) + 1, '\0');
  mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out.size(), &outLen,
                        reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
  out.resize(outLen);
  return out;
}

// Buffered reader over Conn for header lines and chunked bodies.
class Reader {
 public:
  explicit Reader(Conn& conn) : conn(conn) {}

  bool line(std::string& out) {
    out.clear();
    while (true) {
      if (pos == len && !fill()) return false;
      const char c = static_cast<char>(buf[pos++]);
      if (c == '\n') {
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      if (out.size() > 2048) return false;
      out += c;
    }
  }

  // Up to max bytes; 0 at end of stream, -1 on error.
  int some(uint8_t* dst, size_t max) {
    if (pos == len) {
      const int n = conn.read(buf, sizeof(buf));
      if (n <= 0) return n;
      len = static_cast<size_t>(n);
      pos = 0;
    }
    const size_t n = std::min(max, len - pos);
    memcpy(dst, buf + pos, n);
    pos += n;
    return static_cast<int>(n);
  }

 private:
  bool fill() {
    const int n = conn.read(buf, sizeof(buf));
    if (n <= 0) return false;
    len = static_cast<size_t>(n);
    pos = 0;
    return true;
  }

  Conn& conn;
  uint8_t buf[2048];
  size_t pos = 0;
  size_t len = 0;
};

using Sink = std::function<bool(const uint8_t*, size_t)>;
using Total = std::function<void(size_t)>;

// GET url, following redirects; streams a 200 body into sink.
bool get(std::string url, const std::string& user, const std::string& password, const Sink& sink,
         const Total& onTotal, bool* cancel) {
  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    Url u;
    if (!parseUrl(url, u)) {
      LOG_ERR("TSN", "Unsupported URL: %s", url.c_str());
      return false;
    }
    auto conn = std::make_unique<Conn>();
    if (!conn->open(u.host, u.port, u.tls)) return false;

    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                      "\r\nUser-Agent: CrossPoint-homesync\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    if (!user.empty()) req += "Authorization: Basic " + basicAuth(user, password) + "\r\n";
    req += "\r\n";
    if (!conn->writeAll(req.data(), req.size())) return false;

    auto reader = std::make_unique<Reader>(*conn);
    std::string lineText;
    if (!reader->line(lineText) || lineText.size() < 12) return false;
    const int status = atoi(lineText.c_str() + 9);
    long contentLength = -1;
    bool chunked = false;
    std::string location;
    while (reader->line(lineText) && !lineText.empty()) {
      const size_t colon = lineText.find(':');
      if (colon == std::string::npos) continue;
      std::string name = lineText.substr(0, colon);
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return tolower(c); });
      size_t v = colon + 1;
      while (v < lineText.size() && lineText[v] == ' ') ++v;
      const std::string value = lineText.substr(v);
      if (name == "content-length") contentLength = atol(value.c_str());
      if (name == "transfer-encoding" && value.find("chunked") != std::string::npos) chunked = true;
      if (name == "location") location = value;
    }

    if (status >= 300 && status < 400 && !location.empty()) {
      if (location[0] == '/') location = (u.tls ? "https://" : "http://") + u.host + ":" + std::to_string(u.port) + location;
      url = location;
      continue;
    }
    if (status != 200) {
      LOG_ERR("TSN", "HTTP %d for %s", status, url.c_str());
      return false;
    }
    if (onTotal) onTotal(contentLength > 0 ? static_cast<size_t>(contentLength) : 0);

    uint8_t chunk[1024];
    const auto pump = [&](long remaining) -> bool {  // remaining < 0 = until close
      while (remaining != 0) {
        if (cancel && *cancel) return false;
        const size_t want = remaining < 0 ? sizeof(chunk) : std::min<size_t>(sizeof(chunk), remaining);
        const int n = reader->some(chunk, want);
        if (n == 0) return remaining < 0;
        if (n < 0 || !sink(chunk, static_cast<size_t>(n))) return false;
        if (remaining > 0) remaining -= n;
      }
      return true;
    };

    if (!chunked) return pump(contentLength);
    while (true) {
      if (!reader->line(lineText)) return false;
      const long size = strtol(lineText.c_str(), nullptr, 16);
      if (size == 0) return true;
      if (!pump(size) || !reader->line(lineText)) return false;  // trailing CRLF
    }
  }
  LOG_ERR("TSN", "Too many redirects");
  return false;
}

}  // namespace

bool saveAuthKey(const std::string& key) {
  KeyState s = loadKeyState();
  s.key = key;
  return saveKeyState(s);
}

bool configured() {
  const KeyState s = loadKeyState();
  return s.joined || !s.key.empty();
}

bool up(const char* ip, const Status& status, const bool* cancel) {
  serverIp = microlink_parse_ip(ip);
  if (ml && microlink_is_connected(ml)) return true;
  static KeyState keys;  // cfg.auth_key must outlive microlink_init
  if (!ml) {
    keys = loadKeyState();
    if (keys.key.empty() && !keys.joined) return false;
    status("Joining tailnet...");
    microlink_config_t cfg = {};
    cfg.auth_key = keys.key.empty() ? nullptr : keys.key.c_str();
    cfg.device_name = "x4pro-reader";
    cfg.enable_derp = cfg.enable_stun = cfg.enable_disco = true;
    cfg.max_peers = 4;
    cfg.priority_peer_ip = serverIp;
    ml = microlink_init(&cfg);
    if (ml && microlink_start(ml) != ESP_OK) {
      microlink_destroy(ml);
      ml = nullptr;
    }
    if (!ml) {
      LOG_ERR("TSN", "MicroLink start failed");
      return false;
    }
  }
  const uint32_t deadline = millis() + JOIN_TIMEOUT_MS;
  while (!microlink_is_connected(ml) && millis() < deadline) {
    if (cancel && *cancel) return false;
    delay(100);
  }
  const bool ok = microlink_is_connected(ml);
  LOG_INF("TSN", "Tailnet %s", ok ? "connected" : "join timed out");
  if (ok && !keys.joined) {
    keys.joined = true;
    saveKeyState(keys);
  }
  return ok;
}

void down() {
  if (!ml) return;
  microlink_stop(ml);
  microlink_destroy(ml);
  ml = nullptr;
}

bool isUp() { return ml && microlink_is_connected(ml); }

bool fetch(const std::string& url, const HttpDownloader::DataCallback& onData, const std::string& user,
           const std::string& password) {
  if (!isUp()) return false;
  LOG_DBG("TSN", "Fetching: %s", url.c_str());
  return get(url, user, password, onData, nullptr, nullptr);
}

HttpDownloader::DownloadError download(const std::string& url, const std::string& destPath,
                                       const HttpDownloader::ProgressCallback& progress, bool* cancel,
                                       const std::string& user, const std::string& password) {
  if (!isUp()) return HttpDownloader::HTTP_ERROR;
  LOG_DBG("TSN", "Downloading: %s -> %s", url.c_str(), destPath.c_str());
  if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("TSN", destPath.c_str(), file)) return HttpDownloader::FILE_ERROR;
  size_t done = 0;
  size_t total = 0;
  bool fileError = false;
  const bool ok = get(
      url, user, password,
      [&](const uint8_t* data, size_t len) {
        if (file.write(data, len) != len) {
          fileError = true;
          return false;
        }
        done += len;
        if (progress) progress(done, total);
        return true;
      },
      [&](size_t t) { total = t; }, cancel);
  file.close();
  if (ok && (total == 0 || done == total)) return HttpDownloader::OK;
  Storage.remove(destPath.c_str());
  if (cancel && *cancel) return HttpDownloader::ABORTED;
  return fileError ? HttpDownloader::FILE_ERROR : HttpDownloader::HTTP_ERROR;
}

}  // namespace homesync::tailnet

#endif  // HOMESYNC
