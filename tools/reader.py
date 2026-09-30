#!/usr/bin/env python3
"""Drive the x4pro-homesync reader over USB serial (needs pyserial).

  reader.py status
  reader.py screenshot out.png          (macOS: converted with sips)
  reader.py light 60 50                 (brightness 0 = off, warm 0-100)
  reader.py sync
  reader.py setup                       (prompts for Wi-Fi + library; passwords are not echoed)
  reader.py wifi <ssid>                 (prompts for the password)
  reader.py opds <url> <user>           (prompts for the password)
"""
import getpass
import glob
import os
import subprocess
import sys
import tempfile
import time

import serial

WIDTH, HEIGHT = 800, 480  # X4 Pro panel, native orientation


def open_port():
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("Reader not found on USB (awake, and not in USB-drive mode?)")
    return serial.Serial(ports[0], 115200, timeout=0.5)


def command(port, line, expect="HS:", timeout=10.0):
    port.reset_input_buffer()
    port.write((line + "\n").encode())
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += port.read(256)
        for raw in buf.split(b"\n"):
            text = raw.decode(errors="replace").strip()
            if text.startswith(expect):
                return text
    return "(no reply)"


def screenshot(port, path):
    port.reset_input_buffer()
    port.write(b"CMD:SCREENSHOT\n")
    buf = b""
    end = time.time() + 15
    while b"SCREENSHOT_START:" not in buf and time.time() < end:
        buf += port.read(4096)
    head, _, rest = buf.partition(b"SCREENSHOT_START:")
    size_text, _, data = rest.partition(b"\n")
    size = int(size_text)
    while len(data) < size and time.time() < end:
        data += port.read(size - len(data))
    data = data[:size]
    if len(data) != size:
        sys.exit(f"Short screenshot: {len(data)} of {size} bytes")
    width, height = (WIDTH, HEIGHT) if size == WIDTH * HEIGHT // 8 else (size * 8 // HEIGHT, HEIGHT)
    # Framebuffer bit 1 = white; PBM bit 1 = black.
    pbm = f"P4\n{width} {height}\n".encode() + bytes(b ^ 0xFF for b in data)
    with tempfile.NamedTemporaryFile(suffix=".pbm", delete=False) as tmp:
        tmp.write(pbm)
    if path.endswith(".png"):
        subprocess.run(["sips", "-r", "90", "-s", "format", "png", tmp.name, "--out", path], check=True, capture_output=True)
        os.unlink(tmp.name)
    else:
        os.replace(tmp.name, path)
    print(path)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    op, args = sys.argv[1], sys.argv[2:]
    port = open_port()
    if op == "status":
        print(command(port, "CMD:STATUS"))
    elif op == "screenshot":
        screenshot(port, args[0] if args else "reader.png")
    elif op == "light":
        print(command(port, f"CMD:LIGHT {args[0]} {args[1] if len(args) > 1 else -1}"))
    elif op == "sync":
        print(command(port, "CMD:SYNC"))
    elif op in ("wifi", "opds", "setup"):
        if op in ("wifi", "setup"):
            ssid = args[0] if op == "wifi" else input("Wi-Fi name: ")
            pw = getpass.getpass(f"Wi-Fi password for {ssid}: ")
            print(command(port, f"CMD:WIFI_ADD {ssid}\t{pw}"))
        if op in ("opds", "setup"):
            url = args[0] if op == "opds" else (input("Library URL [http://192.168.1.124:8083/opds]: ")
                                                 or "http://192.168.1.124:8083/opds")
            user = args[1] if op == "opds" else (input("User [reader]: ") or "reader")
            pw = getpass.getpass(f"Password for {user}: ")
            print(command(port, f"CMD:OPDS_ADD Home library\t{url}\t{user}\t{pw}"))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
