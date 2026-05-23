#!/usr/bin/env bash
# Deploy the Cloudflare Worker.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

(cd server && npm run deploy -- "$@")
