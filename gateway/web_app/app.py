#!/usr/bin/env python3
"""Temporary shelf master web UI bridged to the Zigbee coordinator serial port."""

from __future__ import annotations

import argparse
import asyncio
import glob
import json
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import serial
from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent))

from protocol import GatewayEvent, format_group_send, format_node_send, parse_gateway_line  # noqa: E402

DEFAULT_GROUP_ID = 0x0001


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

    def request_node_scan(self) -> None:
        try:
            self.send_line("NODE_SCAN\n")
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
        threading.Thread(target=self._scan_loop, name="serial-scan", daemon=True).start()
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
        return {
            "serial_connected": self.serial_connected,
            "coordinator_port": self.port,
            "serial_error": self.serial_error,
            "connected_count": sum(1 for n in nodes if n.online),
            "nodes": [
                {
                    "addr": n.addr,
                    "state": n.state,
                    "online": n.online,
                    "last_seen": n.last_seen,
                }
                for n in nodes
            ],
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
        if not addr.startswith("0x"):
            addr = f"0x{int(addr, 16):04x}"
        self.send_line(format_node_send(addr, state))
        node = self._nodes.get(addr)
        if node is None:
            node = ShelfNode(addr=addr, state=state, online=True, last_seen=time.time())
            self._nodes[addr] = node
        else:
            node.state = state
            node.online = True
            node.last_seen = time.time()
        self._publish()

    def set_group_state(self, state: int) -> None:
        self.send_line(format_group_send(DEFAULT_GROUP_ID, state))
        for node in self._nodes.values():
            if node.online:
                node.state = state
                node.last_seen = time.time()
        self._publish()

    def _scan_loop(self) -> None:
        time.sleep(2)
        while self._running:
            if self.serial_connected:
                self.run_node_scan(wait_seconds=4.0)
            time.sleep(12)

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
            if self._scan_in_progress:
                self._scan_seen_addrs.add(event.addr)
            node = self._nodes.get(event.addr)
            if node is None:
                self._nodes[event.addr] = ShelfNode(addr=event.addr, state=0, online=True, last_seen=now)
            else:
                node.online = True
                node.last_seen = now
        elif event.kind == "leave":
            node = self._nodes.get(event.addr)
            if node:
                node.online = False
                node.last_seen = now
        elif event.kind == "state" and event.state is not None:
            node = self._nodes.get(event.addr)
            if node is None:
                self._nodes[event.addr] = ShelfNode(addr=event.addr, state=event.state, online=True, last_seen=now)
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


bridge: SerialBridge | None = None
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


def main() -> int:
    parser = argparse.ArgumentParser(description="Shelf master web application")
    parser.add_argument(
        "--port",
        default=None,
        help="Coordinator serial port (auto-detected if omitted)",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--web-port", type=int, default=8080)
    args = parser.parse_args()

    port = args.port or discover_coordinator_port()
    if port is None:
        port = args.port or "/dev/cu.usbmodem1201"
        print("Coordinator not found yet — plug coordinator USB into Mac; will auto-reconnect.")
    else:
        print(f"Coordinator serial: {port}")

    global bridge
    bridge = SerialBridge(port, args.baud)

    import uvicorn

    print(f"Shelf master UI: http://localhost:{args.web_port}")
    uvicorn.run(app, host=args.host, port=args.web_port, log_level="info")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
