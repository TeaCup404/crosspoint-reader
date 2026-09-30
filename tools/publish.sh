#!/usr/bin/env bash
# Build x4pro-homesync and publish it to reader-hub on homebot, so the reader
# can install it from Settings -> Check for updates (no USB).
#   tools/publish.sh            build + publish
#   HOST=homebot tools/publish.sh
set -euo pipefail
cd "$(dirname "$0")/.."

HOST=${HOST:-homebot-ts}
OTA_DIR=/srv/fast/reader-hub/ota
KEEP=3

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "Commit first: published builds must match a commit." >&2
  exit 1
fi

count=$(git rev-list --count 707625a..HEAD)
version="1.6.$((100 + count))"
asset="crosspoint-${version}-x4pro.bin"

pio run -e x4pro-homesync
bin=.pio/build/x4pro-homesync/firmware.bin
strings "$bin" | grep -q "${version}-x4pro-hs" || { echo "Built image is not version $version" >&2; exit 1; }
size=$(stat -f%z "$bin")

scp -q "$bin" "$HOST:$OTA_DIR/$asset.tmp"
ssh "$HOST" "set -e; cd $OTA_DIR; mv $asset.tmp $asset
cat > latest.json.tmp <<EOF
{\"tag_name\": \"$version\", \"name\": \"$version\", \"commit\": \"$(git rev-parse --short HEAD)\",
 \"assets\": [{\"name\": \"$asset\", \"size\": $size, \"browser_download_url\": \"\"}]}
EOF
mv latest.json.tmp latest.json
ls -1t crosspoint-*-x4pro.bin | tail -n +$((KEEP + 1)) | xargs -r rm --"
echo "Published $version ($size bytes) as $asset"
