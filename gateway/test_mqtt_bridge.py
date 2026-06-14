"""Tests for Zigbee2MQTT bridge device parsing."""

from __future__ import annotations

import unittest

from mqtt_bridge import Zigbee2MqttBridge, normalize_ieee_address, parse_on_off_state


class TestZigbee2MqttBridge(unittest.TestCase):
    def test_normalize_ieee_address(self) -> None:
        self.assertEqual(normalize_ieee_address("0x0123456789ABCDEF"), "0x0123456789abcdef")
        self.assertEqual(normalize_ieee_address("01:23:45:67:89:ab:cd:ef"), "0x0123456789abcdef")

    def test_parse_on_off_state(self) -> None:
        self.assertEqual(parse_on_off_state({"state": "ON"}), 1)
        self.assertEqual(parse_on_off_state({"state": "OFF"}), 0)
        self.assertIsNone(parse_on_off_state({"linkquality": 100}))

    def test_ingest_device_list_skips_coordinator(self) -> None:
        bridge = Zigbee2MqttBridge()
        bridge._ingest_device_list(
            [
                {"ieee_address": "0xe456acfffe605550", "friendly_name": "Coordinator", "type": "Coordinator"},
                {
                    "ieee_address": "0x1051dbfffe1c4a78",
                    "friendly_name": "shelf_a",
                    "type": "EndDevice",
                    "interview_completed": True,
                    "disabled": False,
                },
            ]
        )
        snapshot = bridge.snapshot()
        self.assertEqual(len(snapshot["nodes"]), 1)
        self.assertEqual(snapshot["nodes"][0]["addr"], "0x1051dbfffe1c4a78")
        self.assertTrue(snapshot["nodes"][0]["online"])

    def test_availability_topic_marks_device_online(self) -> None:
        bridge = Zigbee2MqttBridge()
        bridge._ingest_device_list(
            [
                {
                    "ieee_address": "0x1051dbfffe1c4a78",
                    "friendly_name": "shelf_a",
                    "type": "EndDevice",
                    "interview_completed": True,
                    "disabled": False,
                },
            ]
        )
        bridge._update_device_availability("shelf_a", True)
        snapshot = bridge.snapshot()
        self.assertTrue(snapshot["nodes"][0]["online"])

    def test_group_topic_uses_friendly_name_only(self) -> None:
        bridge = Zigbee2MqttBridge(group_friendly_name="shelves")
        published: list[tuple[str, str]] = []
        bridge.mqtt_connected = True
        bridge._mqtt_client.publish = lambda topic, payload: published.append((topic, payload))  # type: ignore[method-assign]
        bridge.set_group_state(power_state=1)
        self.assertEqual(published[0][0], "zigbee2mqtt/shelves/set")

    def test_not_supported_end_device_still_marked_online(self) -> None:
        bridge = Zigbee2MqttBridge()
        bridge._ingest_device_list(
            [
                {
                    "ieee_address": "0x1051dbfffe1c4a78",
                    "friendly_name": "0x1051dbfffe1c4a78",
                    "type": "EndDevice",
                    "interview_completed": False,
                    "disabled": False,
                },
            ]
        )
        snapshot = bridge.snapshot()
        self.assertTrue(snapshot["nodes"][0]["online"])

    def test_ensure_shelf_device_ready_requests_group_and_configure(self) -> None:
        bridge = Zigbee2MqttBridge(group_friendly_name="shelves")
        published: list[tuple[str, str]] = []
        bridge.mqtt_connected = True
        bridge._mqtt_client.publish = lambda topic, payload: published.append((topic, payload))  # type: ignore[method-assign]
        bridge._ensure_shelf_device_ready("shelf_1")
        topics = [topic for topic, _payload in published]
        self.assertIn("zigbee2mqtt/bridge/request/group/members/add", topics)
        self.assertIn("zigbee2mqtt/bridge/request/device/configure", topics)


if __name__ == "__main__":
    unittest.main()
