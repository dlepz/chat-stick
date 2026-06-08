#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$APP_DIR"

export CHAT_STICK_SERVER_URL="${CHAT_STICK_SERVER_URL:-http://localhost:8787}"
export CHAT_STICK_DEVICE_ID="${CHAT_STICK_DEVICE_ID:-reachy-mini}"
export REACHY_CONNECTION_MODE="${REACHY_CONNECTION_MODE:-localhost_only}"
export REACHY_HOST="${REACHY_HOST:-reachy-mini.local}"
export REACHY_MEDIA_BACKEND="${REACHY_MEDIA_BACKEND:-webrtc}"
export CHAT_STICK_REACHY_MOTION="${CHAT_STICK_REACHY_MOTION:-false}"
export CHAT_STICK_USE_DOA="${CHAT_STICK_USE_DOA:-false}"

if [[ ! -x .venv/bin/python ]]; then
	if command -v uv >/dev/null 2>&1; then
		uv venv
	else
		python3 -m venv .venv
	fi
fi

PY="$APP_DIR/.venv/bin/python"

if ! "$PY" - <<'PY' >/dev/null 2>&1
import numpy
import reachy_mini
import websockets
PY
then
	echo "Installing Reachy chat-stick app dependencies into $APP_DIR/.venv ..."
	"$PY" -m pip install -e .
fi

if ! "$PY" - <<'PY'
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

raw = os.environ["CHAT_STICK_SERVER_URL"]
url = urllib.parse.urlparse(raw)
scheme = {"ws": "http", "wss": "https"}.get(url.scheme, url.scheme or "http")
path = (url.path or "").rstrip("/")
health_url = urllib.parse.urlunparse((scheme, url.netloc, f"{path}/health", "", "", ""))

try:
	with urllib.request.urlopen(health_url, timeout=2) as response:
		body = response.read().decode("utf-8", "replace").strip()
		if response.status == 200 and body == "ok":
			sys.exit(0)
except (urllib.error.URLError, TimeoutError, OSError):
	pass

print(
	f"chat-stick server is not reachable at {health_url}\n"
	"Start it in another terminal with:\n"
	"  cd server && npm run dev\n\n"
	"Or point this app at a deployed worker:\n"
	"  CHAT_STICK_SERVER_URL=https://your-worker.example.workers.dev ./run.sh",
	file=sys.stderr,
)
sys.exit(1)
PY
then
	exit 1
fi

echo "Starting Reachy chat-stick app"
echo "  server:    $CHAT_STICK_SERVER_URL"
echo "  device_id: $CHAT_STICK_DEVICE_ID"
echo "  reachy:    $REACHY_CONNECTION_MODE $REACHY_HOST ($REACHY_MEDIA_BACKEND media)"
echo "  motion:    $CHAT_STICK_REACHY_MOTION"
echo "  doa vad:   $CHAT_STICK_USE_DOA"
exec "$PY" -m reachy_chat_stick.main
