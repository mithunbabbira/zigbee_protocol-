"""JSON gateway protocol for the ESP32 Put-to-Light coordinator."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any, Optional

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


@dataclass
class GatewayEvent:
    kind: str
    ieee: str
    state: Optional[int] = None
    seq: Optional[int] = None
    short_addr: Optional[str] = None

    @property
    def addr(self) -> str:
        return self.ieee


def clean_line(raw: str) -> str:
    return ANSI_ESCAPE.sub("", raw).strip()


def normalize_ieee_address(value: str | None) -> str | None:
    if not value:
        return None
    cleaned = value.strip().lower().replace(":", "")
    if cleaned.startswith("0x"):
        cleaned = cleaned[2:]
    if len(cleaned) != 16:
        return value.lower()
    return f"0x{cleaned}"


def parse_gateway_line(raw: str) -> Optional[GatewayEvent]:
    line = clean_line(raw)
    if not line or not line.startswith("{"):
        return None

    try:
        payload = json.loads(line)
    except json.JSONDecodeError:
        return None

    if not isinstance(payload, dict):
        return None

    event = payload.get("event")
    if isinstance(event, str):
        ieee = normalize_ieee_address(str(payload.get("ieee", "")))
        if ieee is None:
            return None
        if event == "LED_STATE":
            return GatewayEvent("state", ieee, int(payload.get("state", 0)))
        if event == "BUTTON_PRESSED":
            return GatewayEvent("button", ieee)
        if event == "HEARTBEAT":
            return GatewayEvent("heartbeat", ieee, seq=int(payload.get("seq", 0)))
        if event == "NODE_ONLINE":
            short_addr = payload.get("short")
            return GatewayEvent("join", ieee, short_addr=str(short_addr) if short_addr else None)
        if event == "NODE_OFFLINE":
            return GatewayEvent("leave", ieee)

    return None


def format_turn_led_on(ieee: str) -> str:
    return json.dumps({"cmd": "TURN_LED_ON", "ieee": normalize_ieee_address(ieee) or ieee}) + "\n"


def format_turn_led_off(ieee: str) -> str:
    return json.dumps({"cmd": "TURN_LED_OFF", "ieee": normalize_ieee_address(ieee) or ieee}) + "\n"


def format_blink_led(ieee: str, duration_ms: int = 3000) -> str:
    return (
        json.dumps(
            {
                "cmd": "BLINK_LED",
                "ieee": normalize_ieee_address(ieee) or ieee,
                "duration_ms": duration_ms,
            }
        )
        + "\n"
    )


def format_permit_join(duration: int = 180) -> str:
    return json.dumps({"cmd": "PERMIT_JOIN", "duration": duration}) + "\n"


def format_node_send(addr: str, state: int) -> str:
    """Backwards-compatible alias used by tests and legacy callers."""
    return format_turn_led_on(addr) if state else format_turn_led_off(addr)


def format_group_send(group_id: int, state: int) -> str:
    del group_id
    del state
    raise NotImplementedError("Group broadcast removed; control shelves individually by IEEE address")
