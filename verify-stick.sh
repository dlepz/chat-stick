#!/usr/bin/env bash
# Flash and/or capture a serial interaction log from a USB-connected stick.
#
# Usage:
#   ./verify-stick.sh [--flash] [--port /dev/cu.usbmodem101] [--timeout 60]
#
# After the monitor opens, hold A, speak a short phrase, release A, and wait for
# the assistant audio. Press Ctrl-C when finished; the serial log is saved.
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

(cd "$ROOT_DIR/devices/firmware/$DEVICE" && \
  pio device monitor --port "$PORT" --baud 115200) | tee "$LOG_PATH"
