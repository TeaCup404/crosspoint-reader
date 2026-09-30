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
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.environ.get("READER_HUB_ROOT", "/srv/fast/reader-hub")
OTA_DIR = os.path.join(ROOT, "ota")
BIND = os.environ.get("READER_HUB_BIND", "192.168.1.124,100.86.140.113").split(",")
PORT = int(os.environ.get("READER_HUB_PORT", "8790"))
SAFE_NAME = re.compile(r"^[A-Za-z0-9._-]+\.bin$")

# ---- server dashboard for the reader ----
# Composed from homebot-dash's /api/stats (read-only; dash.py is not touched).
# The reader only paints: every string and severity is decided here, so a
# threshold change is an edit to this file, not a reflash.
STATS_URL = os.environ.get("READER_HUB_STATS", "http://127.0.0.1:8787/api/stats")
# Stopped on purpose (Hermes was shut down 2026-09-26); not alarms.
IGNORE = {"WhatsApp bridge", "hermes-gateway"}
OK, WARN, CRIT = 0, 1, 2


def gib(n):
    return f"{n / 2**30:.0f} GB" if n >= 10 * 2**30 else f"{n / 2**30:.1f} GB"


def dash():
    started = time.time()
    with urllib.request.urlopen(STATS_URL, timeout=5) as r:
        d = json.load(r)
    checks, ok = [], 0

    def check(label, detail, value, sev):
        nonlocal ok
        if sev == OK:
            ok += 1
        else:
            checks.append({"label": label, "detail": detail, "value": value, "sev": sev})

    for s in d.get("services", []):
        if s["name"] in IGNORE:
            continue
        check(s["name"], "not responding" if not s["up"] else "", "DOWN" if not s["up"] else "", OK if s["up"] else CRIT)
    for c in d.get("docker", []):
        bad = c["state"] != "running" or c.get("health") == "unhealthy"
        check(c["name"], c.get("status", ""), c["state"].upper(), CRIT if bad else OK)
    for u in d.get("units", []):
        if u["name"] in IGNORE:
            continue
        check(u["name"], u.get("desc", ""), u["active"], OK if u["active"] == "active" else CRIT)
    for f in d.get("system", {}).get("failed", []):
        check(str(f), "systemd unit failed", "FAILED", CRIT)
    root = next((x for x in d.get("disks", []) if x["mount"] == "/"), None)
    for x in d.get("disks", []):
        sev = CRIT if x["percent"] >= 95 else WARN if x["percent"] >= 85 else OK
        check(f"Disk {x['mount']}", f"{gib(x['free'])} free", f"{x['percent']:.0f}%", sev)
    cpu_t = d.get("cpu", {}).get("temp_c")
    if cpu_t is not None:
        check("CPU temperature", "", f"{cpu_t:.0f}°C", CRIT if cpu_t >= 90 else WARN if cpu_t >= 80 else OK)
    gpu = d.get("gpu") or {}
    if gpu.get("temp_c") is not None:
        check("GPU temperature", "", f"{gpu['temp_c']:.0f}°C", WARN if gpu["temp_c"] >= 83 else OK)

    mem, cpu = d["mem"], d["cpu"]
    up = d["host"]["uptime_sec"]
    cards = [
        {"label": "CPU", "value": f"{cpu['percent']:.0f}", "unit": "%",
         "caption": f"{cpu_t:.0f}°C" if cpu_t is not None else f"load {d['host']['load'][0]:.1f}"},
        {"label": "RAM", "value": f"{mem['percent']:.0f}", "unit": "%", "caption": f"{gib(mem['avail'])} free"},
        {"label": "DISK /", "value": f"{root['percent']:.0f}" if root else "?", "unit": "%",
         "caption": f"{gib(root['free'])} free" if root else ""},
    ]
    detail = []
    fast = next((x for x in d.get("disks", []) if x["mount"] == "/srv/fast"), None)
    if fast:
        detail.append({"label": "/srv/fast", "detail": f"{gib(fast['free'])} free", "value": f"{fast['percent']:.0f}%"})
    if gpu.get("name"):
        detail.append({"label": "GPU", "detail": f"{gpu['mem_used'] / 1024:.1f} / {gpu['mem_total'] / 1024:.0f} GB VRAM",
                       "value": f"{gpu.get('util', 0):.0f}%"})
    running = [m["name"] for m in d.get("ollama", {}).get("running", [])]
    detail.append({"label": "Ollama", "detail": ", ".join(running) if running else "idle", "value": str(len(running))})
    detail.append({"label": "Swap", "detail": gib(mem["swap_used"]) + " used", "value": f"{mem['swap_percent']:.0f}%"})

    checks.sort(key=lambda c: -c["sev"])
    days, hours = up // 86400, up % 86400 // 3600
    return {
        "host": d["host"]["hostname"],
        "server_time": int(time.time()),
        "cards": cards,
        "detail": detail,
        "checks": checks,
        "okCount": ok,
        "footer": f"up {days}d {hours}h · {len(d.get('docker', []))} containers · built in {time.time() - started:.1f}s",
    }


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
        elif path == "/dash":
            try:
                body = json.dumps(dash(), ensure_ascii=False).encode()
                self.send_bytes(200, body, "application/json; charset=utf-8")
            except Exception as e:  # stats source down: say so, the reader shows it
                self.send_bytes(502, json.dumps({"error": str(e)}).encode(), "application/json")
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
