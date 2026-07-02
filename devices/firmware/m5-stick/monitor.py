#!/usr/bin/env python3
"""Serial monitor for M5StickS3 — works without a terminal (no curses/termios)."""
import glob
import os
import serial
import sys
import signal

DEFAULT_PORT = "/dev/cu.usbmodem2201"
BAUD = 115200


def detect_port():
    if os.getenv("PORT"):
        return os.getenv("PORT")
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    return candidates[0] if candidates else DEFAULT_PORT

def main():
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    port = detect_port()
    print(f"[monitor] {port} @ {BAUD}", flush=True)
    ser = serial.Serial(port, BAUD, timeout=0.5)
    print("[monitor] connected", flush=True)

    while True:
        data = ser.read(ser.in_waiting or 1)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

if __name__ == "__main__":
    main()
