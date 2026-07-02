#!/usr/bin/env bash
# Publish a new firmware release: bump if needed, build + upload the OTA binary,
# then deploy the Cloudflare Worker.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

./publish-ota-release.sh "$@"
./deploy.sh
