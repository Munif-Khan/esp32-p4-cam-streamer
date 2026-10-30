#!/usr/bin/env python3
"""Capture a single frame from the ESP32-P4 CAM0 stream and save it as a JPEG.

Usage: python tools/capture.py [PORT] [-o out.jpg] [-n frame_index]
"""
import sys
import struct
import argparse
import serial
import serial.tools.list_ports

MAGIC = b'CAM0'
HEADER_FORMAT = '<4sIHHH'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


def find_port():
    for p in serial.tools.list_ports.comports():
        if any(k in p.description for k in ("USB", "Serial", "JTAG", "CDC")):
            return p.device
    ports = list(serial.tools.list_ports.comports())
    return ports[0].device if ports else None


def read_exact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default=None)
    ap.add_argument("-o", "--out", default="capture.jpg")
    ap.add_argument("-n", "--skip", type=int, default=2, help="frames to skip before capture")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        print("No serial port found")
        sys.exit(1)

    print(f"Opening {port} ...")
    ser = serial.Serial(port, 2000000, timeout=1.0)
    ser.reset_input_buffer()

    for attempt in range(3):
        skipped = 0
        while skipped < args.skip:
            b = ser.read(1)
            if not b:
                continue
            if b == MAGIC[:1]:
                rest = ser.read(3)
                if rest == MAGIC[1:]:
                    hdr = read_exact(ser, HEADER_SIZE - 4)
                    if hdr is None:
                        continue
                    magic, length, width, height, fmt = struct.unpack(HEADER_FORMAT, MAGIC + hdr)
                    if length == 0 or length > 5 * 1024 * 1024:
                        continue
                    payload = read_exact(ser, length)
                    if payload is None:
                        continue
                    skipped += 1
                    if skipped >= args.skip:
                        with open(args.out, "wb") as f:
                            f.write(payload)
                        print(f"Saved {args.out}: {width}x{height} fmt={fmt} size={length} bytes")
                        ser.close()
                        return
    print("Timed out waiting for frame")
    sys.exit(2)


if __name__ == "__main__":
    main()
