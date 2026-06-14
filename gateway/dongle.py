"""Discover SONOFF ZBDongle-E / CP210x serial port on macOS."""

from __future__ import annotations

import glob

try:
    import serial.tools.list_ports
except ImportError:
    serial = None

# Silicon Labs CP210x (stock ZBDongle-E USB-UART)
CP210X_VID = 0x10C4
CP210X_PID = 0xEA60


def discover_zigbee_dongle_port() -> str | None:
    if serial is None:
        return None
    for port in serial.tools.list_ports.comports():
        if port.vid == CP210X_VID and port.pid == CP210X_PID:
            return port.device
    for pattern in ("/dev/cu.usbserial-*", "/dev/cu.SLAB_USBtoUART*"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None
