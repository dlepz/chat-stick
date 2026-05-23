#!/usr/bin/env python3
"""Serial monitor for the Waveshare ESP32-S3-Touch-AMOLED-1.8."""
import serial
import sys
import signal

PORT = "/dev/cu.usbmodem101"
BAUD = 115200

def main():
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    print(f"[monitor] {PORT} @ {BAUD}", flush=True)
    ser = serial.Serial(PORT, BAUD, timeout=0.5)
    print("[monitor] connected", flush=True)

    while True:
        data = ser.read(ser.in_waiting or 1)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

if __name__ == "__main__":
    main()
