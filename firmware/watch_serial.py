#!/usr/bin/env python3
"""Continuous timestamped serial monitor for filming/demoing the board (resets board on start; usage: python3 watch_serial.py [port])."""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-5B1F0052761"
ser = serial.Serial(port, 115200, timeout=1)

ser.dtr = False
ser.rts = True
time.sleep(0.15)
ser.rts = False
time.sleep(0.2)

start = time.time()
print(f"--- watching {port}, Ctrl+C to stop ---")
try:
    while True:
        line = ser.readline()
        if line:
            text = line.decode("utf-8", errors="replace").rstrip()
            print(f"[{time.time() - start:7.2f}s] {text}")
except KeyboardInterrupt:
    pass
finally:
    ser.close()
