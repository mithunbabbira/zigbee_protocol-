#!/usr/bin/env python3
"""Phase-2 stub: bridge coordinator UART lines to stdout and accept downstream commands."""

from __future__ import annotations

import argparse
import sys

try:
    import serial
except ImportError:  # pragma: no cover - optional on dev host
    serial = None


def run(port: str, baud: int) -> int:
    if serial is None:
        print("pyserial is required: pip install pyserial", file=sys.stderr)
        return 1

    with serial.Serial(port, baudrate=baud, timeout=0.1) as ser:
        print(f"Listening on {port} @ {baud}. Type NODE_SEND/GROUP_SEND lines.", flush=True)
        while True:
            if ser.in_waiting:
                line = ser.readline().decode("utf-8", errors="replace").rstrip()
                if line:
                    print(line, flush=True)

            if sys.stdin in select_wait():
                cmd = sys.stdin.readline()
                if not cmd:
                    return 0
                ser.write(cmd.encode("utf-8"))
                if not cmd.endswith("\n"):
                    ser.write(b"\n")


def select_wait():
    import select

    return select.select([sys.stdin], [], [], 0)[0]


def main() -> int:
    parser = argparse.ArgumentParser(description="Shelf coordinator serial gateway")
    parser.add_argument("port", help="Serial port, e.g. /dev/cu.usbmodem101")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    return run(args.port, args.baud)


if __name__ == "__main__":
    raise SystemExit(main())
