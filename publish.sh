#!/usr/bin/env bash
# Publish a new firmware release: build + upload the OTA binary, then deploy
# the Cloudflare Worker. Run after bumping FIRMWARE_VERSION in
# firmware/src/Config.h.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

./publish-ota-release.sh
./deploy.sh
