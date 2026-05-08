#!/usr/bin/env bash
# Build and flash firmware to a USB-connected Waveshare ESP32-S3-Touch-AMOLED-1.8.
# Configure upload_port/monitor_port in firmware/platformio.ini first.
#
# Pass --monitor to also open the serial monitor after flashing.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

MONITOR=false
for arg in "$@"; do
  case $arg in
    --monitor) MONITOR=true ;;
  esac
done

(cd firmware && pio run -t upload)

if $MONITOR; then
  (cd firmware && pio device monitor)
fi
