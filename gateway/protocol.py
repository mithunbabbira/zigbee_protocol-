"""UART gateway line parsing for the shelf coordinator."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Optional

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
NODE_RECV_RE = re.compile(r"NODE_RECV:(0x[0-9A-Fa-f]+):STATE:([01])")
NODE_JOIN_RE = re.compile(r"NODE_JOIN:(0x[0-9A-Fa-f]+)")
NODE_LEAVE_RE = re.compile(r"NODE_LEAVE:(0x[0-9A-Fa-f]+)")


@dataclass
class GatewayEvent:
    kind: str
    addr: str
    state: Optional[int] = None


def clean_line(raw: str) -> str:
    return ANSI_ESCAPE.sub("", raw).strip()


def parse_gateway_line(raw: str) -> Optional[GatewayEvent]:
    line = clean_line(raw)
    if not line:
        return None

    match = NODE_RECV_RE.search(line)
    if match:
        return GatewayEvent("state", match.group(1).lower(), int(match.group(2)))

    match = NODE_JOIN_RE.search(line)
    if match:
        return GatewayEvent("join", match.group(1).lower())

    match = NODE_LEAVE_RE.search(line)
    if match:
        return GatewayEvent("leave", match.group(1).lower())

    return None


def format_node_send(addr: str, state: int) -> str:
    if not addr.startswith("0x"):
        addr = f"0x{int(addr, 16):04x}"
    return f"NODE_SEND:{addr}:STATE:{1 if state else 0}\n"


def format_group_send(group_id: int, state: int) -> str:
    return f"GROUP_SEND:0x{group_id:04x}:STATE:{1 if state else 0}\n"
