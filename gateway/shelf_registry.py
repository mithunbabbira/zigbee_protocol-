"""Persistent shelf ID assignment and button-press counters (IEEE -> shelf row)."""

from __future__ import annotations

import json
import threading
from pathlib import Path
from typing import Any

DEFAULT_REGISTRY_PATH = Path(__file__).resolve().parent / "web_app" / "data" / "shelf_registry.json"


class ShelfRegistry:
    """Maps connected ESP32 IEEE addresses to incrementing shelf IDs and press counts."""

    def __init__(self, path: Path | None = None) -> None:
        self._path = path or DEFAULT_REGISTRY_PATH
        self._lock = threading.Lock()
        self._nodes: dict[str, dict[str, int]] = {}
        self._next_shelf_id = 1
        self._load()

    def ensure_node(self, ieee_address: str) -> dict[str, int]:
        with self._lock:
            record = self._nodes.get(ieee_address)
            if record is None:
                record = {"shelf_id": self._next_shelf_id, "press_count": 0}
                self._next_shelf_id += 1
                self._nodes[ieee_address] = record
                self._save()
            return dict(record)

    def record_button_press(self, ieee_address: str) -> dict[str, int]:
        with self._lock:
            record = self._nodes.get(ieee_address)
            if record is None:
                record = {"shelf_id": self._next_shelf_id, "press_count": 0}
                self._next_shelf_id += 1
                self._nodes[ieee_address] = record
            record["press_count"] += 1
            self._save()
            return dict(record)

    def clear_press_counts(self, ieee_address: str | None = None) -> None:
        with self._lock:
            if ieee_address is None:
                for record in self._nodes.values():
                    record["press_count"] = 0
            else:
                key = str(ieee_address).lower()
                record = self._nodes.get(key)
                if record is not None:
                    record["press_count"] = 0
            self._save()

    def enrich(self, ieee_address: str, node: dict[str, Any]) -> dict[str, Any]:
        meta = self.ensure_node(ieee_address)
        return {
            **node,
            "shelf_id": meta["shelf_id"],
            "press_count": meta["press_count"],
        }

    def _load(self) -> None:
        if not self._path.exists():
            return
        try:
            raw = json.loads(self._path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            return
        nodes = raw.get("nodes")
        if not isinstance(nodes, dict):
            return
        loaded: dict[str, dict[str, int]] = {}
        max_id = 0
        for ieee, record in nodes.items():
            if not isinstance(record, dict):
                continue
            shelf_id = int(record.get("shelf_id", 0))
            press_count = int(record.get("press_count", 0))
            if shelf_id <= 0:
                continue
            loaded[str(ieee).lower()] = {"shelf_id": shelf_id, "press_count": press_count}
            max_id = max(max_id, shelf_id)
        self._nodes = loaded
        self._next_shelf_id = max_id + 1 if loaded else 1

    def _save(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        payload = {"next_shelf_id": self._next_shelf_id, "nodes": self._nodes}
        self._path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
