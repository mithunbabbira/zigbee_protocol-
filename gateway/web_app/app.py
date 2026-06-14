#!/usr/bin/env python3
"""Temporary shelf master web UI bridged to the Zigbee coordinator serial port."""

from __future__ import annotations

import argparse
import asyncio
import glob
import json
import socket
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import serial
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

ROOT = Path(__file__).resolve().parent
CERT_DIR = ROOT / "certs"
DEV_SSL_KEY = CERT_DIR / "dev-key.pem"
DEV_SSL_CERT = CERT_DIR / "dev-cert.pem"
sys.path.insert(0, str(ROOT.parent))

from protocol import (  # noqa: E402
    GatewayEvent,
    format_blink_led,
    format_permit_join,
    format_turn_led_off,
    format_turn_led_on,
    normalize_ieee_address,
    parse_gateway_line,
)
from shelf_registry import ShelfRegistry  # noqa: E402

try:
    from mqtt_bridge import MqttBridge  # noqa: E402
except ImportError:
    sys.path.insert(0, str(ROOT.parent))
    from mqtt_bridge import MqttBridge  # noqa: E402

try:
    from dongle import discover_zigbee_dongle_port  # noqa: E402
except ImportError:
    sys.path.insert(0, str(ROOT.parent))
    from dongle import discover_zigbee_dongle_port  # noqa: E402

DEFAULT_GROUP_ID = 0x0001  # legacy constant retained for API compatibility


def probe_port_role(port: str) -> str | None:
    """Return 'coordinator', 'shelf', or None by inspecting recent serial output."""
    try:
        with serial.Serial(port, 115200, timeout=0.2) as ser:
            time.sleep(0.2)
            data = ser.read(4096).decode("utf-8", errors="replace")
            if "COORDINATOR" in data or "Project name:     coordinator" in data:
                return "coordinator"
            if "SHELF_NODE" in data or "Project name:     shelf_node" in data:
                return "shelf"

            ser.setDTR(False)
            ser.setRTS(False)
            time.sleep(0.05)
            ser.setDTR(True)
            ser.setRTS(True)
            time.sleep(2.5)
            data += ser.read(12000).decode("utf-8", errors="replace")
    except (serial.SerialException, OSError):
        return None

    if "COORDINATOR" in data or "Project name:     coordinator" in data:
        return "coordinator"
    if "SHELF_NODE" in data or "Project name:     shelf_node" in data:
        return "shelf"
    return None


def discover_coordinator_port(preferred: str | None = None) -> str | None:
    if preferred and Path(preferred).exists():
        role = probe_port_role(preferred)
        if role == "coordinator":
            return preferred
        if role == "shelf":
            preferred = None

    for port in sorted(glob.glob("/dev/cu.usbmodem*")):
        if port == preferred:
            continue
        if probe_port_role(port) == "coordinator":
            return port
    return None


@dataclass
class ShelfNode:
    addr: str
    state: int = 0
    online: bool = True
    last_seen: float = field(default_factory=time.time)


class SerialBridge:
    def __init__(self, port: str, baud: int = 115200) -> None:
        self.port = port
        self.baud = baud
        self._ser: serial.Serial | None = None
        self._lock = threading.Lock()
        self._nodes: dict[str, ShelfNode] = {}
        self._listeners: list[asyncio.Queue[dict[str, Any]]] = []
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self._running = False
        self.serial_connected = False
        self.serial_error = ""
        self._scan_in_progress = False
        self._scan_seen_addrs: set[str] = set()
        self._registry = ShelfRegistry()

    def request_node_scan(self) -> None:
        try:
            self.send_line(format_permit_join(180))
        except (RuntimeError, serial.SerialException, OSError):
            pass

    def run_node_scan(self, wait_seconds: float = 4.0) -> None:
        with self._lock:
            self._scan_seen_addrs = set()
            self._scan_in_progress = True
        self.request_node_scan()
        time.sleep(wait_seconds)
        self._finalize_scan()

    def _finalize_scan(self) -> None:
        with self._lock:
            if not self._scan_in_progress:
                return
            self._scan_in_progress = False
            now = time.time()
            for addr, node in self._nodes.items():
                if addr not in self._scan_seen_addrs:
                    node.online = False
                    node.last_seen = now
        self._publish()

    def start(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop
        self._running = True
        self._try_connect()
        self._thread = threading.Thread(target=self._reader_loop, name="serial-reader", daemon=True)
        self._thread.start()
        threading.Thread(target=self._heartbeat_watchdog, name="serial-watchdog", daemon=True).start()
        threading.Thread(target=self._reconnect_loop, name="serial-reconnect", daemon=True).start()

    def stop(self) -> None:
        self._running = False
        self._close_serial()

    def _close_serial(self) -> None:
        was_connected = self.serial_connected
        self.serial_connected = False
        if self._ser is not None:
            try:
                if self._ser.is_open:
                    self._ser.close()
            except (serial.SerialException, OSError):
                pass
            self._ser = None
        if was_connected:
            self._publish()

    def _open_serial(self) -> None:
        self._close_serial()
        if not Path(self.port).exists():
            raise OSError(f"Coordinator port not found: {self.port}")
        self._ser = serial.Serial(self.port, self.baud, timeout=0.2)
        self.serial_connected = True
        self.serial_error = ""
        self._publish()

    def _try_connect(self) -> bool:
        try:
            self._open_serial()
            return True
        except (serial.SerialException, OSError) as exc:
            self.serial_error = str(exc)
            self._publish()
            return False

    def _ensure_serial(self) -> None:
        if self._ser is not None and self._ser.is_open:
            return
        if not self._try_connect():
            raise OSError(self.serial_error or "Serial port is not open")

    def _reconnect_loop(self) -> None:
        while self._running:
            if not self.serial_connected:
                found = discover_coordinator_port(self.port)
                if found and found != self.port:
                    self.port = found
                    print(f"Coordinator discovered on {found}")
                if found:
                    self._try_connect()
            time.sleep(5)

    def snapshot(self) -> dict[str, Any]:
        nodes = sorted(self._nodes.values(), key=lambda n: n.addr)
        rows = []
        total_presses = 0
        for node in nodes:
            row = self._registry.enrich(
                node.addr,
                {
                    "addr": node.addr,
                    "state": node.state,
                    "online": node.online,
                    "last_seen": node.last_seen,
                },
            )
            total_presses += row["press_count"]
            rows.append(row)
        rows.sort(key=lambda row: row["shelf_id"])
        return {
            "backend": "serial",
            "serial_connected": self.serial_connected,
            "coordinator_port": self.port,
            "serial_error": self.serial_error,
            "connected_count": sum(1 for n in nodes if n.online),
            "total_press_count": total_presses,
            "nodes": rows,
        }

    def subscribe(self) -> asyncio.Queue[dict[str, Any]]:
        queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._listeners.append(queue)
        return queue

    def unsubscribe(self, queue: asyncio.Queue[dict[str, Any]]) -> None:
        if queue in self._listeners:
            self._listeners.remove(queue)

    def send_line(self, line: str) -> None:
        last_error: Exception | None = None
        with self._lock:
            for _ in range(3):
                try:
                    self._ensure_serial()
                    assert self._ser is not None
                    self._ser.write(line.encode("utf-8"))
                    self._ser.flush()
                    return
                except (serial.SerialException, OSError) as exc:
                    last_error = exc
                    self._close_serial()
                    time.sleep(0.2)
        raise RuntimeError(f"Serial write failed: {last_error}")

    def set_node_state(self, addr: str, state: int) -> None:
        ieee = normalize_ieee_address(addr) or addr
        line = format_turn_led_on(ieee) if state else format_turn_led_off(ieee)
        self.send_line(line)
        node = self._nodes.get(ieee)
        if node is not None:
            node.state = state
            node.last_seen = time.time()
        self._publish()

    def set_group_state(self, state: int) -> None:
        for node in list(self._nodes.values()):
            if node.online:
                self.set_node_state(node.addr, state)

    def clear_press_counts(self, ieee_address: str | None = None) -> None:
        self._registry.clear_press_counts(ieee_address)
        self._publish()

    def _heartbeat_watchdog(self) -> None:
        """Mark nodes offline when no heartbeat/state for 90 s."""
        while self._running:
            time.sleep(15)
            now = time.time()
            changed = False
            with self._lock:
                for node in self._nodes.values():
                    if node.online and now - node.last_seen > 90:
                        node.online = False
                        changed = True
            if changed:
                self._publish()

    def _reader_loop(self) -> None:
        buffer = ""
        while self._running:
            try:
                self._ensure_serial()
                assert self._ser is not None
                chunk = self._ser.read(512)
            except (serial.SerialException, OSError):
                self._close_serial()
                time.sleep(0.5)
                continue
            if not chunk:
                continue
            buffer += chunk.decode("utf-8", errors="replace")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                self._handle_line(line)

    def _handle_line(self, raw_line: str) -> None:
        event = parse_gateway_line(raw_line)
        if event is None:
            return
        self._apply_event(event)
        self._publish()

    def _apply_event(self, event: GatewayEvent) -> None:
        now = time.time()
        if event.kind == "join":
            ieee = normalize_ieee_address(event.addr) or event.addr
            if self._scan_in_progress:
                self._scan_seen_addrs.add(ieee)
            node = self._nodes.get(ieee)
            if node is None:
                self._nodes[ieee] = ShelfNode(addr=ieee, state=0, online=True, last_seen=now)
            else:
                node.online = True
                node.last_seen = now
            self._registry.ensure_node(ieee)
        elif event.kind == "leave":
            ieee = normalize_ieee_address(event.addr) or event.addr
            node = self._nodes.get(ieee)
            if node:
                node.online = False
                node.last_seen = now
        elif event.kind == "button":
            ieee = normalize_ieee_address(event.addr) or event.addr
            node = self._nodes.get(ieee)
            if node is None:
                node = ShelfNode(addr=ieee, state=1, online=True, last_seen=now)
                self._nodes[ieee] = node
            else:
                node.state = 1 if node.state == 0 else 0
                node.online = True
                node.last_seen = now
            self._registry.record_button_press(ieee)
        elif event.kind == "heartbeat":
            ieee = normalize_ieee_address(event.addr) or event.addr
            node = self._nodes.get(ieee)
            if node is None:
                self._nodes[ieee] = ShelfNode(addr=ieee, state=0, online=True, last_seen=now)
            else:
                node.online = True
                node.last_seen = now
        elif event.kind == "state" and event.state is not None:
            ieee = normalize_ieee_address(event.addr) or event.addr
            node = self._nodes.get(ieee)
            if node is None:
                self._nodes[ieee] = ShelfNode(addr=ieee, state=event.state, online=True, last_seen=now)
            else:
                node.state = event.state
                node.online = True
                node.last_seen = now

    def _publish(self) -> None:
        if self._loop is None:
            return
        payload = self.snapshot()
        for queue in list(self._listeners):
            asyncio.run_coroutine_threadsafe(queue.put(payload), self._loop)


bridge: SerialBridge | MqttBridge | None = None
app = FastAPI(title="Shelf Master")
static_dir = ROOT / "static"
if static_dir.is_dir():
    app.mount("/static", StaticFiles(directory=static_dir), name="static")


class NodeStateBody(BaseModel):
    state: int = Field(ge=0, le=1)


class GroupStateBody(BaseModel):
    state: int = Field(ge=0, le=1)


@app.on_event("startup")
async def on_startup() -> None:
    global bridge
    if bridge is None:
        raise RuntimeError("Serial bridge not configured")
    bridge.start(asyncio.get_running_loop())


@app.on_event("shutdown")
async def on_shutdown() -> None:
    if bridge:
        bridge.stop()


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(static_dir / "index.html")


@app.get("/api/state")
async def api_state() -> dict[str, Any]:
    assert bridge is not None
    return bridge.snapshot()


@app.post("/api/scan")
async def api_scan() -> dict[str, Any]:
    assert bridge is not None
    await asyncio.to_thread(bridge.run_node_scan, 4.0)
    return bridge.snapshot()


@app.get("/api/events")
async def api_events() -> StreamingResponse:
    assert bridge is not None
    queue = bridge.subscribe()

    async def event_stream():
        try:
            yield f"data: {json.dumps(bridge.snapshot())}\n\n"
            while True:
                payload = await queue.get()
                yield f"data: {json.dumps(payload)}\n\n"
        finally:
            bridge.unsubscribe(queue)

    return StreamingResponse(event_stream(), media_type="text/event-stream")


@app.post("/api/nodes/{addr}/state")
async def api_set_node_state(addr: str, body: NodeStateBody) -> dict[str, Any]:
    assert bridge is not None
    try:
        bridge.set_node_state(addr, body.state)
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return bridge.snapshot()


@app.post("/api/group/state")
async def api_set_group_state(body: GroupStateBody) -> dict[str, Any]:
    assert bridge is not None
    try:
        bridge.set_group_state(body.state)
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return bridge.snapshot()


@app.post("/api/press-counts/clear")
async def api_clear_press_counts() -> dict[str, Any]:
    assert bridge is not None
    bridge.clear_press_counts()
    return bridge.snapshot()


def _detect_lan_ip() -> str | None:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        return None


@app.get("/api/mobile-info")
async def api_mobile_info(request: Request) -> dict[str, Any]:
    port = request.url.port or 8080
    lan_ip = _detect_lan_ip()
    scheme = request.url.scheme
    phone_url = f"https://{lan_ip}:{port}" if lan_ip else None
    return {
        "lan_ip": lan_ip,
        "port": port,
        "scheme": scheme,
        "is_secure": scheme == "https",
        "phone_url": phone_url,
        "camera_requires_https": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Shelf master web application")
    parser.add_argument(
        "--backend",
        choices=("mqtt", "serial"),
        default="serial",
        help="Coordinator backend: serial (ESP32-C6 coordinator JSON gateway) or mqtt (legacy Zigbee2MQTT)",
    )
    parser.add_argument(
        "--port",
        default=None,
        help="Coordinator serial port (serial backend only; auto-detected if omitted)",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mqtt-host", default="127.0.0.1")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-group", default="shelves", help="Zigbee2MQTT group friendly name")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--web-port", type=int, default=8080)
    args = parser.parse_args()

    global bridge
    if args.backend == "mqtt":
        dongle = discover_zigbee_dongle_port()
        if dongle:
            print(f"Zigbee dongle detected: {dongle} (used by Zigbee2MQTT, not this app directly)")
        else:
            print("Zigbee dongle not detected on USB — start Zigbee2MQTT after plugging in SONOFF dongle.")
        bridge = MqttBridge(
            mqtt_host=args.mqtt_host,
            mqtt_port=args.mqtt_port,
            group_friendly_name=args.mqtt_group,
        )
        print(f"MQTT backend: mqtt://{args.mqtt_host}:{args.mqtt_port}")
    else:
        port = args.port or discover_coordinator_port()
        if port is None:
            port = args.port or "/dev/cu.usbmodem1201"
            print("Coordinator not found yet — plug coordinator USB into Mac; will auto-reconnect.")
        else:
            print(f"Coordinator serial: {port}")
        bridge = SerialBridge(port, args.baud)

    import uvicorn

    ssl_keyfile: str | None = None
    ssl_certfile: str | None = None
    if DEV_SSL_KEY.is_file() and DEV_SSL_CERT.is_file():
        ssl_keyfile = str(DEV_SSL_KEY)
        ssl_certfile = str(DEV_SSL_CERT)

    lan_ip = _detect_lan_ip()
    if ssl_certfile:
        print(f"Shelf master UI: https://localhost:{args.web_port}")
        if lan_ip:
            print(f"Phone (QR scan):  https://{lan_ip}:{args.web_port}")
            print("Accept the self-signed certificate warning on first visit.")
    else:
        print(f"Shelf master UI: http://localhost:{args.web_port}")
        print("Mobile QR scan needs HTTPS — run: ./scripts/ensure_dev_ssl.sh")

    uvicorn.run(
        app,
        host=args.host,
        port=args.web_port,
        log_level="info",
        ssl_keyfile=ssl_keyfile,
        ssl_certfile=ssl_certfile,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
