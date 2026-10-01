#!/usr/bin/env python3
"""Mirror the CrossPoint SD fonts (.cpfont) to reader-hub's font directory.

The x4pro-homesync firmware ships only Noto Serif 14 built in; every other
family and size is a download from reader-hub (/fonts/fonts.json). This copies
the families that cover Latin or Hebrew from the upstream fonts release (the
Korean and Arabic-only families are skipped: ~100 MB nobody here reads), checks
each file's CRC32, and writes the filtered manifest. Re-running only fetches
files that are missing or changed.

    python3 fonts_mirror.py [dest_dir]
"""
import json
import os
import sys
import urllib.request
import zlib

# Must match FONTS_MANIFEST_VERSION / CPFONT_VERSION in the firmware.
UPSTREAM = "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m1-b4/fonts.json"
DEST = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.environ.get("READER_HUB_ROOT", "/srv/fast/reader-hub"), "fonts")
SKIP_SCRIPTS = {"hangul", "arabic"}


def wanted(family):
    scripts = set(family.get("scripts", []))
    return bool(scripts & {"latin", "hebrew"}) and not scripts & SKIP_SCRIPTS


def crc_of(path):
    crc = 0
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            crc = zlib.crc32(chunk, crc)
    return crc


def main():
    os.makedirs(DEST, exist_ok=True)
    with urllib.request.urlopen(UPSTREAM, timeout=60) as r:
        manifest = json.load(r)
    base = manifest["baseUrl"]
    families = [f for f in manifest["families"] if wanted(f)]
    total = 0
    for family in families:
        for entry in family["files"]:
            path = os.path.join(DEST, entry["name"])
            if os.path.exists(path) and os.path.getsize(path) == entry["size"] and crc_of(path) == entry["crc32"]:
                continue
            tmp = path + ".part"
            with urllib.request.urlopen(base + entry["name"], timeout=120) as r, open(tmp, "wb") as out:
                while chunk := r.read(1 << 16):
                    out.write(chunk)
            if crc_of(tmp) != entry["crc32"]:
                os.remove(tmp)
                raise SystemExit(f"CRC mismatch: {entry['name']}")
            os.replace(tmp, path)
            total += entry["size"]
            print(f"  {entry['name']}", flush=True)
    manifest["families"] = families
    manifest["scriptGroups"] = [g for g in manifest.get("scriptGroups", []) if g["tag"] not in SKIP_SCRIPTS]
    with open(os.path.join(DEST, "fonts.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"{len(families)} families, {total / 1e6:.1f} MB downloaded -> {DEST}")


if __name__ == "__main__":
    main()
