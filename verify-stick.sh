#!/usr/bin/env bash
# Flash and/or capture a serial interaction log from a USB-connected stick.
#
# Usage:
#   ./verify-stick.sh [--flash] [--expect-pass] [--port /dev/cu.usbmodem101] [--timeout 60]
#                    [--capture-timeout 120]
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
CAPTURE_TIMEOUT_SECONDS="${VERIFY_CAPTURE_SECONDS:-0}"
EXPECT_PASS=false

usage() {
  echo "Usage: $0 [--flash] [--expect-pass] [--port /dev/cu.usbmodem101] [--timeout 60] [--capture-timeout 120]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --flash)
      FLASH=true
      ;;
    --expect-pass)
      EXPECT_PASS=true
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
    --capture-timeout)
      shift
      [[ $# -gt 0 ]] || { usage; exit 1; }
      CAPTURE_TIMEOUT_SECONDS="$1"
      ;;
    --capture-timeout=*)
      CAPTURE_TIMEOUT_SECONDS="${1#--capture-timeout=}"
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
if $EXPECT_PASS; then
  echo "  3. Wait for VERIFY_PASS. The script exits non-zero if the sequence is incomplete."
else
  echo "  3. Wait for Speaking / turn complete / Ready, then press Ctrl-C."
fi
echo

VERIFY_PORT="$PORT" \
VERIFY_LOG_PATH="$LOG_PATH" \
VERIFY_CAPTURE_TIMEOUT_SECONDS="$CAPTURE_TIMEOUT_SECONDS" \
VERIFY_EXPECT_PASS="$EXPECT_PASS" \
python3 - <<'PY'
import os
import select
import sys
import termios
import time
from datetime import datetime

port = os.environ["VERIFY_PORT"]
log_path = os.environ["VERIFY_LOG_PATH"]
capture_timeout = float(os.environ.get("VERIFY_CAPTURE_TIMEOUT_SECONDS", "0") or "0")
expect_pass = os.environ.get("VERIFY_EXPECT_PASS", "false").lower() == "true"

checks = [
    ("ready", lambda text: "Loop - state=1" in text and "ws=1" in text),
    ("recording_start", lambda text: "Recording - start" in text),
    ("recording_audio_sent", lambda text: "Recording - #" in text and " sent bytes=" in text),
    ("recording_stop", lambda text: "Recording - stop" in text),
    ("user_transcript", lambda text: "Transcript - user:" in text),
    ("assistant_audio", lambda text: "Playback - audio len=" in text and "queued=1" in text),
    ("playing", lambda text: "State - Thinking -> Playing" in text or "status=Speaking" in text),
    ("turn_complete", lambda text: "turn complete" in text),
    ("ready_after_playback", lambda text: "State - Playing -> Ready" in text),
]
next_check = 0


def note_check(text):
    global next_check
    if not expect_pass or next_check >= len(checks):
        return False
    name, matcher = checks[next_check]
    if matcher(text):
        print(f"VERIFY_STEP {next_check + 1}/{len(checks)} {name}", flush=True)
        next_check += 1
        if next_check >= len(checks):
            print(f"VERIFY_PASS log={log_path}", flush=True)
            return True
    return False


def missing_checks():
    return ", ".join(name for name, _ in checks[next_check:]) or "none"

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
    started_at = time.time()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        while True:
            if capture_timeout > 0 and time.time() - started_at >= capture_timeout:
                if expect_pass:
                    print(
                        f"\nVERIFY_FAIL missing: {missing_checks()}; log={log_path}",
                        file=sys.stderr,
                    )
                    sys.exit(2)
                print(f"\nSerial capture timed out; log saved to {log_path}")
                break
            try:
                readable, _, _ = select.select([fd], [], [], 0.2)
            except KeyboardInterrupt:
                if expect_pass and next_check < len(checks):
                    print(
                        f"\nVERIFY_FAIL interrupted; missing: {missing_checks()}; log={log_path}",
                        file=sys.stderr,
                    )
                    sys.exit(130)
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
                if note_check(text):
                    sys.exit(0)
finally:
    os.close(fd)
PY
