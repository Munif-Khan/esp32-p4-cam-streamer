#!/usr/bin/env python3
"""
ESP32-P4 USB Video Viewer
Reads framed camera stream from USB Serial and displays in real-time OpenCV window.
"""

import sys
import time
import struct
import serial
import serial.tools.list_ports
import numpy as np
import cv2

MAGIC = b'CAM0'
HEADER_FORMAT = '<4sIHHH'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

def find_default_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "USB" in p.description or "Serial" in p.description or "JTAG" in p.description:
            return p.device
    if ports:
        return ports[0].device
    return "COM4"

def read_exact(ser, num_bytes):
    buf = bytearray()
    while len(buf) < num_bytes:
        chunk = ser.read(num_bytes - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_default_port()
    baud = 2000000

    print(f"[*] Opening serial port {port}...")
    try:
        ser = serial.Serial(port, baudrate=baud, timeout=1.0)
    except Exception as e:
        print(f"[!] Error opening port {port}: {e}")
        sys.exit(1)

    print("[*] Connected! Waiting for video stream...")
    print("[*] Press 'q' or ESC in the video window to exit.")

    fps_count = 0
    fps_start = time.time()
    current_fps = 0.0

    cv2.namedWindow("ESP32-P4 Camera Stream", cv2.WINDOW_NORMAL)

    try:
        while True:
            # Sync to magic bytes 'CAM0'
            b = ser.read(1)
            if not b:
                continue
            if b == b'C':
                rest = ser.read(3)
                if rest == b'AM0':
                    # Found header start, read rest of header
                    hdr_bytes = read_exact(ser, HEADER_SIZE - 4)
                    if hdr_bytes is None:
                        continue
                    full_hdr = MAGIC + hdr_bytes
                    magic, length, width, height, fmt = struct.unpack(HEADER_FORMAT, full_hdr)

                    if length == 0 or length > 5 * 1024 * 1024:
                        continue

                    # Read payload
                    payload = read_exact(ser, length)
                    if payload is None or len(payload) != length:
                        continue

                    frame = None
                    if fmt == 2: # JPEG
                        nparr = np.frombuffer(payload, np.uint8)
                        frame = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
                    elif fmt == 1: # RGB565
                        raw = np.frombuffer(payload, dtype=np.uint16)
                        if len(raw) == width * height:
                            raw = raw.reshape((height, width))
                            # Convert RGB565 to BGR for OpenCV
                            frame = cv2.cvtColor(raw, cv2.COLOR_BGR5652BGR)
                    elif fmt == 3: # GRAY8
                        raw = np.frombuffer(payload, dtype=np.uint8)
                        if len(raw) == width * height:
                            frame = raw.reshape((height, width))

                    if frame is not None:
                        fps_count += 1
                        now = time.time()
                        if now - fps_start >= 1.0:
                            current_fps = fps_count / (now - fps_start)
                            fps_count = 0
                            fps_start = now

                        # Overlay FPS & Resolution
                        info_text = f"{frame.shape[1]}x{frame.shape[0]} | {current_fps:.1f} FPS"
                        cv2.putText(frame, info_text, (10, 30),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2, cv2.LINE_AA)

                        cv2.imshow("ESP32-P4 Camera Stream", frame)
                        key = cv2.waitKey(1) & 0xFF
                        if key == ord('q') or key == 27:
                            break
    except KeyboardInterrupt:
        print("\n[*] Stopped by user.")
    finally:
        ser.close()
        cv2.destroyAllWindows()
        print("[*] Closed port and window.")

if __name__ == '__main__':
    main()
