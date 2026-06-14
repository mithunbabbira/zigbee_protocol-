#!/usr/bin/env bash
# Remove a stale device from Zigbee2MQTT (e.g. old test pairings).
set -euo pipefail
IEEE="${1:?Usage: $0 0x1051dbfffe1c4a78}"
mosquitto_pub -h 127.0.0.1 -t zigbee2mqtt/bridge/request/device/remove \
  -m "{\"id\":\"${IEEE}\",\"force\":true}"
echo "Remove requested for ${IEEE} — check Zigbee2MQTT logs or Shelf Master at http://localhost:8080"
