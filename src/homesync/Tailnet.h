#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "network/HttpDownloader.h"

/**
 * Tailscale for the home library sync (x4pro-homesync fork).
 *
 * MicroLink is not a network interface: it opens TCP connections to tailnet
 * IPs through its own API, so esp_http_client and the Wi-Fi stack's DNS never
 * see the tunnel. This module does the small amount of HTTP(S) the sync needs
 * on top of it: wolfSSL over a MicroLink socket, verified against the Let's
 * Encrypt roots, with the tailnet name mapped to a fixed tailnet IP.
 */
namespace homesync::tailnet {

using Status = std::function<void(const char*)>;

// Auth key storage: /.crosspoint/tailnet.json, key XOR-obfuscated with the
// device MAC like the Wi-Fi/OPDS passwords. `joined` records a first
// successful login (after that the identity in NVS is enough).
bool saveAuthKey(const std::string& key);
bool configured();  // a key was saved or the device has joined before

// Joins the tailnet (identity is kept in NVS after the first login; authKey is
// only needed until then). Blocks up to ~60 s. Wi-Fi and the clock must be up.
bool up(const char* serverIp, const Status& status, const bool* cancel);
void down();
bool isUp();

// Same contracts as HttpDownloader, but over the tunnel. Only http:// and
// https:// URLs whose host is `hostName` (or a literal IP) are supported; the
// host resolves to `serverIp` given to up().
// ---- routing for HttpDownloader (all HTTP in the firmware goes through it) ----
// Maps a home-LAN URL to its tailnet equivalent (CWA -> https://books.gabyhome.xyz,
// reader-hub -> http://100.86.140.113:8790). URLs already on the tailnet map to
// themselves with `direct` set. False for anything else (internet, other hosts).
bool tailnetUrlFor(const std::string& url, std::string& out, bool& direct);
// True when Wi-Fi is on the home LAN (192.168.1.0/24).
bool atHome();
// Test hook (CMD:ROUTE TS): behave as if away from home until reboot.
extern bool forceAway;
// Generic GET over the tunnel: streams the 200 body to onData, reports the
// Content-Length (0 if unknown) through onTotal. Tailnet must be up.
bool get(const std::string& url, const std::string& user, const std::string& password,
         const std::function<bool(const uint8_t*, size_t)>& onData, const std::function<void(size_t)>& onTotal,
         bool* cancel);
// One request over the tunnel (no redirects). `headers` is raw "Name: value\r\n"
// lines; any HTTP status is reported, the body is kept up to maxBody bytes.
bool request(const char* method, const std::string& url, const std::string& headers, const std::string& body,
             int& status, std::string& response, size_t maxBody = 4096);
constexpr const char* HOMEBOT_TAILNET_IP = "100.86.140.113";

bool fetch(const std::string& url, const HttpDownloader::DataCallback& onData, const std::string& user,
           const std::string& password);
HttpDownloader::DownloadError download(const std::string& url, const std::string& destPath,
                                       const HttpDownloader::ProgressCallback& progress, bool* cancel,
                                       const std::string& user, const std::string& password);

}  // namespace homesync::tailnet
