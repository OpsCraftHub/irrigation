#!/usr/bin/env python3
import serial
import time
import sys

try:
    ser = serial.Serial('/dev/cu.SLAB_USBtoUART', 115200, timeout=1)
    print("Connected to ESP32. Monitoring output for 10 seconds...")
    print("-" * 60)

    start_time = time.time()
    while time.time() - start_time < 10:
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            print(data, end='')
            sys.stdout.flush()
        time.sleep(0.1)

    print("\n" + "-" * 60)
    print("Monitoring complete.")
    ser.close()

except Exception as e:
    print(f"Error: {e}")
    sys.exit(1)
