#!/usr/bin/env bash
# Build the current firmware and publish it to R2 for OTA distribution.
# Devices on an older version will pick this up on next boot.
#
# Releasing a new version:
#   1. Bump FIRMWARE_VERSION in firmware/src/Config.h
#   2. ./publish-ota-release.sh
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

CONFIG_H="firmware/src/Config.h"
FIRMWARE_BIN="firmware/.pio/build/waveshare-esp32-s3-touch-amoled-1_8/firmware.bin"
BUCKET="m5-stick-assets"
KEY_PREFIX="chat-stick/firmware"

VERSION=$(grep -E "constexpr int FIRMWARE_VERSION" "$CONFIG_H" | grep -oE "[0-9]+" | head -1)
if [[ -z "$VERSION" ]]; then
  echo "Error: could not parse FIRMWARE_VERSION from $CONFIG_H" >&2
  exit 1
fi

KEY="$KEY_PREFIX/firmware-v$VERSION.bin"

echo "Publishing firmware v$VERSION → $BUCKET/$KEY"
echo

echo "[1/2] Building firmware..."
(cd firmware && pio run)

if [[ ! -f "$FIRMWARE_BIN" ]]; then
  echo "Error: $FIRMWARE_BIN not found after build" >&2
  exit 1
fi

SIZE=$(wc -c < "$FIRMWARE_BIN" | tr -d ' ')

echo
echo "[2/2] Uploading $SIZE bytes to R2..."
(cd server && npx wrangler r2 object put "$BUCKET/$KEY" --file="../$FIRMWARE_BIN" --remote)

echo
echo "Published firmware v$VERSION ($SIZE bytes)"
echo "Devices on older versions will install this on next boot."
