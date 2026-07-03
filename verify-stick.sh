#!/usr/bin/env bash
# Flash and/or capture a serial interaction log from a USB-connected stick.
#
# Usage:
#   ./verify-stick.sh [--flash] [--port /dev/cu.usbmodem101] [--timeout 60]
#
# After the serial capture opens, hold A, speak a short phrase, release A, and
# wait for the assistant audio. Press Ctrl-C when finished; the serial log is
# saved.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE="m5-stick"
PORT="${FIRMWARE_PORT:-${PORT:-}}"
FLASH=false
TIMEOUT_SECONDS="${VERIFY_WAIT_SECONDS:-0}"

usage() {
  echo "Usage: $0 [--flash] [--port /dev/cu.usbmodem101] [--timeout 60]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --flash)
      FLASH=true
      ;;
    --port)
      shift
      [[ $# -gt 0 ]] || { usage; exit 1; }
      PORT="$1"
      ;;
    --port=*)
      PORT="${1#--port=}"
      ;;
    --timeout)
      shift
      [[ $# -gt 0 ]] || { usage; exit 1; }
      TIMEOUT_SECONDS="$1"
      ;;
    --timeout=*)
      TIMEOUT_SECONDS="${1#--timeout=}"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      exit 1
      ;;
  esac
  shift
done

find_port() {
  shopt -s nullglob
  local candidates=(/dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial*)
  shopt -u nullglob
  if [[ ${#candidates[@]} -eq 1 ]]; then
    printf '%s\n' "${candidates[0]}"
    return 0
  fi
  if [[ ${#candidates[@]} -gt 1 ]]; then
    printf 'multiple:%s\n' "${candidates[*]}"
    return 0
  fi
  return 1
}

if [[ -z "$PORT" ]]; then
  started_at="$(date +%s)"
  while true; do
    detected="$(find_port || true)"
    if [[ "$detected" == multiple:* ]]; then
      echo "Multiple ESP serial ports found: ${detected#multiple:}" >&2
      echo "Retry with --port /dev/cu.usbmodem..." >&2
      exit 1
    fi
    if [[ -n "$detected" ]]; then
      PORT="$detected"
      break
    fi
    if [[ "$TIMEOUT_SECONDS" != "0" ]]; then
      now="$(date +%s)"
      if (( now - started_at >= TIMEOUT_SECONDS )); then
        echo "No ESP serial port found before timeout." >&2
        exit 1
      fi
    fi
    echo "Waiting for ESP serial port... (wake/replug the stick, Ctrl-C to cancel)" >&2
    sleep 1
  done
fi

if $FLASH; then
  "$ROOT_DIR/flash.sh" "$DEVICE" --port "$PORT"
  sleep 2
fi

LOG_PATH="$ROOT_DIR/serial-interaction-$(date +%Y%m%d-%H%M%S).log"

echo "Using $PORT"
echo "Saving serial log to $LOG_PATH"
echo
echo "When the monitor opens:"
echo "  1. Wait for Ready / ws=1."
echo "  2. Hold A, speak: 'hello can you hear me', then release A."
echo "  3. Wait for Speaking / turn complete / Ready, then press Ctrl-C."
echo

VERIFY_PORT="$PORT" VERIFY_LOG_PATH="$LOG_PATH" python3 - <<'PY'
import os
import select
import sys
import termios
import time
from datetime import datetime

port = os.environ["VERIFY_PORT"]
log_path = os.environ["VERIFY_LOG_PATH"]

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = attrs[2] | termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    buffer = b""
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        while True:
            try:
                readable, _, _ = select.select([fd], [], [], 0.2)
            except KeyboardInterrupt:
                print(f"\nSerial capture stopped; log saved to {log_path}")
                break
            if not readable:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            except OSError as exc:
                print(f"\nSerial read stopped: {exc}", file=sys.stderr)
                break
            if not chunk:
                time.sleep(0.05)
                continue
            buffer += chunk
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace").rstrip("\r")
                line = f"{datetime.now().strftime('%H:%M:%S')} {text}"
                print(line, flush=True)
                log.write(line + "\n")
                log.flush()
finally:
    os.close(fd)
PY
