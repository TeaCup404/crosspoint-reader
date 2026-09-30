#!/usr/bin/env python3
"""reader-hub: small homebot service for the X4 Pro reader (stdlib only).

Serves firmware updates in the shape CrossPoint's OtaUpdater expects from
GitHub's "latest release" API:

  GET /ota/latest.json        {"tag_name": ..., "assets": [{"name", "browser_download_url", "size"}]}
  GET /ota/<file>.bin         the firmware image

The firmware directory holds the published .bin files plus latest.json, which
tools/publish.sh writes. Bound to the LAN and tailnet addresses only.
"""
import json
import os
import re
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.environ.get("READER_HUB_ROOT", "/srv/fast/reader-hub")
OTA_DIR = os.path.join(ROOT, "ota")
BIND = os.environ.get("READER_HUB_BIND", "192.168.1.124,100.86.140.113").split(",")
PORT = int(os.environ.get("READER_HUB_PORT", "8790"))
SAFE_NAME = re.compile(r"^[A-Za-z0-9._-]+\.bin$")


class Handler(BaseHTTPRequestHandler):
    server_version = "reader-hub/1"

    def log_message(self, fmt, *args):
        print(f"{self.client_address[0]} {fmt % args}", flush=True)

    def send_bytes(self, code, body, content_type):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/health":
            self.send_bytes(200, b"ok\n", "text/plain")
        elif path == "/ota/latest.json":
            self.latest()
        elif path.startswith("/ota/") and SAFE_NAME.match(path[5:]):
            self.firmware(path[5:])
        else:
            self.send_bytes(404, b"not found\n", "text/plain")

    def latest(self):
        try:
            with open(os.path.join(OTA_DIR, "latest.json"), "rb") as f:
                release = json.load(f)
        except (OSError, ValueError):
            self.send_bytes(404, b'{"message":"no release"}', "application/json")
            return
        # Download URLs follow the address the reader used (LAN or tailnet).
        host = self.headers.get("Host") or f"{BIND[0]}:{PORT}"
        for asset in release.get("assets", []):
            asset["browser_download_url"] = f"http://{host}/ota/{asset['name']}"
        self.send_bytes(200, json.dumps(release).encode(), "application/json")

    def firmware(self, name):
        full = os.path.join(OTA_DIR, name)
        try:
            size = os.path.getsize(full)
            f = open(full, "rb")
        except OSError:
            self.send_bytes(404, b"not found\n", "text/plain")
            return
        with f:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            while chunk := f.read(64 * 1024):
                self.wfile.write(chunk)


def serve(address):
    httpd = ThreadingHTTPServer((address, PORT), Handler)
    print(f"reader-hub on {address}:{PORT}, ota dir {OTA_DIR}", flush=True)
    httpd.serve_forever()


def main():
    os.makedirs(OTA_DIR, exist_ok=True)
    threads = []
    for address in BIND:
        try:
            socket.inet_aton(address)
        except OSError:
            continue
        t = threading.Thread(target=serve, args=(address,), daemon=True)
        t.start()
        threads.append(t)
    for t in threads:
        t.join()


if __name__ == "__main__":
    main()
