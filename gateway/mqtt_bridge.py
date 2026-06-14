"""Bridge between the shelf web UI and Zigbee2MQTT over MQTT.

Zigbee2MQTT topic reference:
  - Device list : zigbee2mqtt/bridge/devices
  - Device state: zigbee2mqtt/<friendly_name>
  - Set state   : zigbee2mqtt/<friendly_name>/set  payload {"state":"ON"|"OFF"}
  - Group set   : zigbee2mqtt/<group_friendly_name>/set
  - Permit join : zigbee2mqtt/bridge/request/permit_join  payload {"time": seconds}
"""

from __future__ import annotations

import asyncio
import json
import threading
import time
from dataclasses import dataclass, field
from typing import Any

import paho.mqtt.client as mqtt

from shelf_registry import ShelfRegistry

DEFAULT_MQTT_HOST = "127.0.0.1"
DEFAULT_MQTT_PORT = 1883
DEFAULT_TOPIC_PREFIX = "zigbee2mqtt"
DEFAULT_GROUP_FRIENDLY_NAME = "shelves"
DEFAULT_SHELF_ENDPOINT = "10"
COMMAND_SPACING_SECONDS = 0.4
OUTBOUND_ECHO_WINDOW_SECONDS = 2.0


def normalize_ieee_address(value: str | None) -> str | None:
    """Convert IEEE addresses to a stable lowercase 0x-prefixed form."""
    if not value:
        return None
    cleaned = value.strip().lower().replace(":", "")
    if cleaned.startswith("0x"):
        cleaned = cleaned[2:]
    if len(cleaned) != 16:
        return value.lower()
    return f"0x{cleaned}"


def parse_on_off_state(payload: dict[str, Any]) -> int | None:
    """Return 1 for ON, 0 for OFF, or None when state is absent."""
    raw_state = payload.get("state_light", payload.get("state"))
    if raw_state is None:
        return None
    return 1 if str(raw_state).upper() == "ON" else 0


@dataclass
class ShelfNode:
    ieee_address: str
    friendly_name: str = ""
    power_state: int = 0
    is_online: bool = False
    last_seen_monotonic: float = field(default_factory=time.time)

    @property
    def addr(self) -> str:
        """Alias used by the web UI JSON schema."""
        return self.ieee_address


class Zigbee2MqttBridge:
    """Keeps shelf node state in sync with Zigbee2MQTT MQTT topics."""

    def __init__(
        self,
        mqtt_host: str = DEFAULT_MQTT_HOST,
        mqtt_port: int = DEFAULT_MQTT_PORT,
        topic_prefix: str = DEFAULT_TOPIC_PREFIX,
        group_friendly_name: str = DEFAULT_GROUP_FRIENDLY_NAME,
    ) -> None:
        self.mqtt_host = mqtt_host
        self.mqtt_port = mqtt_port
        self.topic_prefix = topic_prefix.rstrip("/")
        self.group_friendly_name = group_friendly_name

        self._mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self._mqtt_client.on_connect = self._handle_mqtt_connect
        self._mqtt_client.on_message = self._handle_mqtt_message

        self._state_lock = threading.Lock()
        self._nodes_by_ieee: dict[str, ShelfNode] = {}
        self._ieee_by_friendly_name: dict[str, str] = {}

        self._event_loop: asyncio.AbstractEventLoop | None = None
        self._mqtt_thread: threading.Thread | None = None
        self._should_run = False
        self._command_lock = threading.Lock()
        self._last_command_monotonic = 0.0
        self._group_members_added: set[str] = set()
        self._devices_configured: set[str] = set()
        self._availability_by_ieee: dict[str, bool] = {}
        # IEEE addresses of interviewed SHELF-NODE devices only (not stale Z2M pairings).
        self._registered_shelf_ieee: set[str] = set()
        self._registry = ShelfRegistry()
        self._last_outbound_command: dict[str, tuple[int, float]] = {}

        self.mqtt_connected = False
        self.mqtt_error = ""
        self.coordinator_online = False
        self._event_subscribers: list[asyncio.Queue[dict[str, Any]]] = []

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self, event_loop: asyncio.AbstractEventLoop) -> None:
        self._event_loop = event_loop
        self._should_run = True
        self._mqtt_thread = threading.Thread(target=self._run_mqtt_loop, name="zigbee2mqtt-bridge", daemon=True)
        self._mqtt_thread.start()

    def stop(self) -> None:
        self._should_run = False
        try:
            self._mqtt_client.disconnect()
        except Exception:  # noqa: BLE001
            pass
        self.mqtt_connected = False

    def subscribe(self) -> asyncio.Queue[dict[str, Any]]:
        queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue(maxsize=64)
        self._event_subscribers.append(queue)
        return queue

    def unsubscribe(self, queue: asyncio.Queue[dict[str, Any]]) -> None:
        if queue in self._event_subscribers:
            self._event_subscribers.remove(queue)

    # ------------------------------------------------------------------
    # Public API used by FastAPI routes
    # ------------------------------------------------------------------

    def snapshot(self) -> dict[str, Any]:
        with self._state_lock:
            nodes = sorted(
                (
                    node
                    for node in self._nodes_by_ieee.values()
                    if node.ieee_address in self._registered_shelf_ieee
                ),
                key=lambda node: node.ieee_address,
            )
            online_nodes = [node for node in nodes if node.is_online]
            total_presses = 0
            node_rows = []
            for node in nodes:
                row = self._registry.enrich(
                    node.ieee_address,
                    {
                        "addr": node.ieee_address,
                        "friendly_name": node.friendly_name,
                        "state": node.power_state,
                        "online": node.is_online,
                        "last_seen": node.last_seen_monotonic,
                    },
                )
                total_presses += row["press_count"]
                node_rows.append(row)
            node_rows.sort(key=lambda row: row["shelf_id"])
            return {
                "backend": "zigbee2mqtt",
                "serial_connected": self.mqtt_connected and self.coordinator_online,
                "coordinator_port": f"mqtt://{self.mqtt_host}:{self.mqtt_port}",
                "serial_error": self.mqtt_error,
                "connected_count": len(online_nodes),
                "total_press_count": total_presses,
                "nodes": node_rows,
            }

    def run_node_scan(self, wait_seconds: float = 3.0) -> None:
        self.permit_join(int(wait_seconds) if wait_seconds >= 60 else 180)
        self.request_device_refresh()
        time.sleep(wait_seconds)

    def request_device_refresh(self) -> None:
        if not self.mqtt_connected:
            return
        self._mqtt_client.publish(f"{self.topic_prefix}/bridge/request/devices", payload="")
        self._mqtt_client.publish(f"{self.topic_prefix}/bridge/request/health_check", payload="")

    def set_node_state(self, ieee_address: str, power_state: int) -> None:
        if not self.mqtt_connected:
            raise RuntimeError("MQTT broker is not connected")
        normalized = normalize_ieee_address(ieee_address) or ieee_address
        self._last_outbound_command[normalized] = (power_state, time.monotonic())
        friendly_name = self._resolve_friendly_name(ieee_address)
        payload = json.dumps({"state_light": "ON" if power_state else "OFF"})
        self._publish_device_set(friendly_name, payload)

    def set_group_state(self, power_state: int) -> None:
        if not self.mqtt_connected:
            raise RuntimeError("MQTT broker is not connected")
        now = time.monotonic()
        with self._state_lock:
            for ieee in self._registered_shelf_ieee:
                self._last_outbound_command[ieee] = (power_state, now)
        payload = json.dumps({"state_light": "ON" if power_state else "OFF"})
        self._publish_device_set(self.group_friendly_name, payload)

    def clear_press_counts(self, ieee_address: str | None = None) -> None:
        self._registry.clear_press_counts(ieee_address)
        self._notify_subscribers()

    def permit_join(self, duration_seconds: int = 180) -> None:
        if not self.mqtt_connected:
            return
        payload = json.dumps({"time": duration_seconds})
        self._mqtt_client.publish(f"{self.topic_prefix}/bridge/request/permit_join", payload=payload)

    # ------------------------------------------------------------------
    # MQTT internals
    # ------------------------------------------------------------------

    def _run_mqtt_loop(self) -> None:
        while self._should_run:
            try:
                self._mqtt_client.connect(self.mqtt_host, self.mqtt_port, keepalive=60)
                self._mqtt_client.loop_start()
                while self._should_run and self._mqtt_client.is_connected():
                    time.sleep(0.5)
                self._mqtt_client.loop_stop()
            except Exception as exc:  # noqa: BLE001
                self.mqtt_connected = False
                self.mqtt_error = str(exc)
                self._notify_subscribers()
                time.sleep(2)

    def _handle_mqtt_connect(
        self,
        client: mqtt.Client,
        userdata: Any,
        connect_flags: Any,
        reason_code: Any,
        properties: Any = None,
    ) -> None:
        del userdata, connect_flags, properties
        if getattr(reason_code, "value", reason_code) != 0:
            self.mqtt_connected = False
            self.mqtt_error = f"MQTT connect failed ({reason_code})"
            self._notify_subscribers()
            return

        self.mqtt_connected = True
        self.mqtt_error = ""
        client.subscribe(f"{self.topic_prefix}/#")
        self.request_device_refresh()
        self._notify_subscribers()

    def _handle_mqtt_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        del client, userdata
        payload_text = message.payload.decode("utf-8", errors="replace").strip()
        if not payload_text:
            return

        try:
            payload = json.loads(payload_text)
        except json.JSONDecodeError:
            return

        topic = message.topic
        bridge_state_topic = f"{self.topic_prefix}/bridge/state"
        bridge_devices_topic = f"{self.topic_prefix}/bridge/devices"

        if topic == bridge_state_topic:
            self._handle_coordinator_state(payload)
            return
        if topic == bridge_devices_topic:
            self._ingest_device_list(payload)
            return
        bridge_event_topic = f"{self.topic_prefix}/bridge/event"
        if topic == bridge_event_topic:
            self._handle_bridge_event(payload)
            return
        if topic.endswith("/availability"):
            friendly_name = topic.rsplit("/", 2)[-2]
            self._update_device_availability(friendly_name, payload.get("state") == "online")
            return

        device_name = self._extract_device_topic_name(topic)
        if device_name is not None:
            self._update_device_power_state(device_name, payload)

    def _handle_coordinator_state(self, payload: Any) -> None:
        if isinstance(payload, dict):
            self.coordinator_online = payload.get("state") == "online"
        else:
            self.coordinator_online = payload == "online"
        self._notify_subscribers()

    def _ingest_device_list(self, devices_payload: Any) -> None:
        if not isinstance(devices_payload, list):
            return

        now = time.time()
        seen_shelf_ieee: set[str] = set()
        with self._state_lock:
            for device_record in devices_payload:
                if not isinstance(device_record, dict):
                    continue
                if self._should_skip_device(device_record):
                    continue

                ieee_address = normalize_ieee_address(device_record.get("ieee_address"))
                if ieee_address is None:
                    continue

                seen_shelf_ieee.add(ieee_address)
                self._registered_shelf_ieee.add(ieee_address)
                self._registry.ensure_node(ieee_address)
                friendly_name = str(device_record.get("friendly_name") or ieee_address)
                node = self._nodes_by_ieee.get(ieee_address)
                if node is None:
                    node = ShelfNode(ieee_address=ieee_address, friendly_name=friendly_name)
                    self._nodes_by_ieee[ieee_address] = node
                else:
                    node.friendly_name = friendly_name

                node.is_online = self._derive_online_flag(device_record)
                node.last_seen_monotonic = now
                self._ieee_by_friendly_name[friendly_name] = ieee_address
                known_availability = self._availability_by_ieee.get(ieee_address)
                if known_availability is not None:
                    node.is_online = known_availability
                self._ensure_shelf_device_ready(friendly_name)

            for ieee in list(self._nodes_by_ieee.keys()):
                if ieee not in seen_shelf_ieee:
                    friendly = self._nodes_by_ieee[ieee].friendly_name
                    del self._nodes_by_ieee[ieee]
                    if friendly in self._ieee_by_friendly_name:
                        del self._ieee_by_friendly_name[friendly]
                    self._availability_by_ieee.pop(ieee, None)
            self._registered_shelf_ieee.intersection_update(seen_shelf_ieee)

        self._notify_subscribers()

    def _handle_bridge_event(self, payload: Any) -> None:
        if not isinstance(payload, dict):
            return

        event_type = payload.get("type")
        data = payload.get("data")
        if not isinstance(data, dict):
            return

        if event_type == "device_interview" and data.get("status") == "successful":
            definition = data.get("definition")
            if not isinstance(definition, dict):
                return
            if definition.get("model") != "SHELF-NODE" and definition.get("vendor") != "SHELF_MGMT":
                return
            ieee_address = normalize_ieee_address(str(data.get("ieee_address", "")))
            if ieee_address is None:
                return
            friendly_name = str(data.get("friendly_name") or ieee_address)
            with self._state_lock:
                self._registered_shelf_ieee.add(ieee_address)
                self._registry.ensure_node(ieee_address)
                node = self._nodes_by_ieee.get(ieee_address)
                if node is None:
                    node = ShelfNode(ieee_address=ieee_address, friendly_name=friendly_name, is_online=True)
                    self._nodes_by_ieee[ieee_address] = node
                else:
                    node.friendly_name = friendly_name
                    node.is_online = True
                node.last_seen_monotonic = time.time()
                self._ieee_by_friendly_name[friendly_name] = ieee_address
            self._ensure_shelf_device_ready(friendly_name)
            self._notify_subscribers()
            return

        if event_type == "device_leave":
            ieee_address = normalize_ieee_address(str(data.get("ieee_address", "")))
            if ieee_address is None:
                return
            with self._state_lock:
                self._registered_shelf_ieee.discard(ieee_address)
                node = self._nodes_by_ieee.pop(ieee_address, None)
                self._availability_by_ieee.pop(ieee_address, None)
                if node and node.friendly_name in self._ieee_by_friendly_name:
                    del self._ieee_by_friendly_name[node.friendly_name]
                self._group_members_added.discard(node.friendly_name if node else "")
                self._devices_configured.discard(node.friendly_name if node else "")
            self._notify_subscribers()

    def _update_device_availability(self, friendly_name: str, is_online: bool) -> None:
        with self._state_lock:
            ieee_address = self._lookup_ieee_address(friendly_name)
            if ieee_address not in self._registered_shelf_ieee:
                return
            self._availability_by_ieee[ieee_address] = is_online
            node = self._nodes_by_ieee.get(ieee_address)
            if node is None:
                node = ShelfNode(ieee_address=ieee_address, friendly_name=friendly_name, is_online=is_online)
                self._nodes_by_ieee[ieee_address] = node
                self._ieee_by_friendly_name[friendly_name] = ieee_address
            else:
                node.is_online = is_online
            node.last_seen_monotonic = time.time()
        self._notify_subscribers()

    def _update_device_power_state(self, friendly_name: str, payload: dict[str, Any]) -> None:
        power_state = parse_on_off_state(payload)
        if power_state is None:
            return

        now = time.time()
        with self._state_lock:
            ieee_address = self._lookup_ieee_address(friendly_name)
            if ieee_address not in self._registered_shelf_ieee:
                return
            node = self._nodes_by_ieee.get(ieee_address)
            if node is None:
                node = ShelfNode(
                    ieee_address=ieee_address,
                    friendly_name=friendly_name,
                    power_state=power_state,
                    is_online=True,
                )
                self._nodes_by_ieee[ieee_address] = node
                self._ieee_by_friendly_name[friendly_name] = ieee_address
            else:
                previous_state = node.power_state
                node.power_state = power_state
                node.friendly_name = friendly_name
                if self._is_button_press(ieee_address, previous_state, power_state):
                    self._registry.record_button_press(ieee_address)
            node.last_seen_monotonic = now
        self._notify_subscribers()

    def _is_button_press(self, ieee_address: str, previous_state: int, new_state: int) -> bool:
        if previous_state == new_state:
            return False
        command = self._last_outbound_command.get(ieee_address)
        if command is None:
            return True
        commanded_state, sent_at = command
        if time.monotonic() - sent_at <= OUTBOUND_ECHO_WINDOW_SECONDS and commanded_state == new_state:
            return False
        return True

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _should_skip_device(device_record: dict[str, Any]) -> bool:
        device_type = str(device_record.get("type") or "")
        if device_type in ("Coordinator", "Group"):
            return True
        friendly_name = str(device_record.get("friendly_name") or "")
        if friendly_name in ("shelves", DEFAULT_GROUP_FRIENDLY_NAME):
            return True
        return not Zigbee2MqttBridge._is_shelf_device(device_record)

    @staticmethod
    def _is_shelf_device(device_record: dict[str, Any]) -> bool:
        definition = device_record.get("definition")
        if isinstance(definition, dict):
            if definition.get("model") == "SHELF-NODE":
                return True
            if definition.get("vendor") == "SHELF_MGMT":
                return True
        return False

    def _ensure_shelf_device_ready(self, friendly_name: str) -> None:
        if not self.mqtt_connected:
            return
        if friendly_name not in self._group_members_added:
            payload = json.dumps(
                {
                    "group": self.group_friendly_name,
                    "device": friendly_name,
                    "endpoint": DEFAULT_SHELF_ENDPOINT,
                }
            )
            self._mqtt_client.publish(f"{self.topic_prefix}/bridge/request/group/members/add", payload=payload)
            self._group_members_added.add(friendly_name)
        if friendly_name not in self._devices_configured:
            payload = json.dumps({"id": friendly_name})
            self._mqtt_client.publish(f"{self.topic_prefix}/bridge/request/device/configure", payload=payload)
            self._devices_configured.add(friendly_name)

    def _publish_device_set(self, friendly_name: str, payload: str) -> None:
        with self._command_lock:
            elapsed = time.monotonic() - self._last_command_monotonic
            if elapsed < COMMAND_SPACING_SECONDS:
                time.sleep(COMMAND_SPACING_SECONDS - elapsed)
            self._mqtt_client.publish(f"{self.topic_prefix}/{friendly_name}/set", payload=payload)
            self._last_command_monotonic = time.monotonic()

    @staticmethod
    def _derive_online_flag(device_record: dict[str, Any]) -> bool:
        """Shelf nodes are online when interviewed and supported."""
        if device_record.get("disabled"):
            return False
        if not Zigbee2MqttBridge._is_shelf_device(device_record):
            return False
        return bool(device_record.get("interview_completed"))

    def _extract_device_topic_name(self, topic: str) -> str | None:
        prefix = f"{self.topic_prefix}/"
        if not topic.startswith(prefix):
            return None
        remainder = topic[len(prefix) :]
        if "/" in remainder:
            return None
        if remainder.startswith("bridge") or remainder.startswith("group"):
            return None
        if remainder in (self.group_friendly_name, "shelves"):
            return None
        return remainder

    def _lookup_ieee_address(self, friendly_name: str) -> str:
        mapped = self._ieee_by_friendly_name.get(friendly_name)
        if mapped is not None:
            return mapped
        normalized = normalize_ieee_address(friendly_name)
        return normalized or friendly_name

    def _resolve_friendly_name(self, ieee_address: str) -> str:
        with self._state_lock:
            node = self._nodes_by_ieee.get(ieee_address)
            if node and node.friendly_name:
                return node.friendly_name
            for candidate in self._nodes_by_ieee.values():
                if candidate.ieee_address == ieee_address or candidate.friendly_name == ieee_address:
                    return candidate.friendly_name or candidate.ieee_address
        return ieee_address

    def _notify_subscribers(self) -> None:
        if self._event_loop is None:
            return
        payload = self.snapshot()
        for queue in list(self._event_subscribers):
            asyncio.run_coroutine_threadsafe(queue.put(payload), self._event_loop)


# Backwards-compatible alias used by app.py imports.
MqttBridge = Zigbee2MqttBridge
