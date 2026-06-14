#!/usr/bin/env bash
# Mac test stack: SONOFF Zigbee dongle (coordinator) + ESP32-C6 shelf node.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHELF_PORT="${1:-}"

pkill -f "web_app/app.py" 2>/dev/null || true

if [[ -n "${SHELF_PORT}" ]]; then
  echo "Flashing shelf_node to ${SHELF_PORT}..."
  source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" 2>/dev/null || source "$HOME/.espressif/esp-idf/export.sh"
  "${ROOT}/scripts/flash_shelf.sh" end_device "${SHELF_PORT}"
  IEEE="$(python3 - <<PY 2>/dev/null || true
import subprocess, re
out = subprocess.check_output(
    ["python", "-m", "esptool", "--chip", "esp32c6", "-p", "${SHELF_PORT}", "read_mac"],
    text=True, stderr=subprocess.STDOUT,
)
m = re.search(r"^MAC: ([0-9a-f:]+)$", out, re.M | re.I)
if not m:
    raise SystemExit(0)
mac = m.group(1).lower().split(":")
if len(mac) == 8:
    print("0x" + "".join(mac))
elif len(mac) == 6:
    print("0x" + "".join(mac[:3]) + "fffe" + "".join(mac[3:]))
PY
)"
  if [[ -n "${IEEE}" ]]; then
    echo "Removing stale Zigbee2MQTT entry for ${IEEE} (if any)..."
    mosquitto_pub -h 127.0.0.1 -t zigbee2mqtt/bridge/request/device/remove -m "{\"id\":\"${IEEE}\",\"force\":true}" 2>/dev/null || true
  fi
  echo "Shelf flashed. Place board near SONOFF dongle and press RESET after Zigbee2MQTT is up."
fi

if ! lsof -i :1883 >/dev/null 2>&1; then
  echo "Starting Mosquitto..."
  /opt/homebrew/opt/mosquitto/sbin/mosquitto -c "${ROOT}/gateway/mosquitto.conf" &
  sleep 1
fi

if ! pgrep -f "zigbee2mqtt.*/index.js" >/dev/null 2>&1; then
  echo "Starting Zigbee2MQTT (headless)..."
  (cd "${ROOT}" && ./scripts/run_zigbee2mqtt_native.sh) &
  sleep 8
else
  echo "Zigbee2MQTT already running"
fi

if [[ -n "${SHELF_PORT}" ]]; then
  mosquitto_pub -h 127.0.0.1 -t zigbee2mqtt/bridge/request/permit_join -m '{"time":180}' 2>/dev/null || true
fi

export SHELF_BACKEND=mqtt
echo "Shelf Master UI: https://localhost:8080 (phone QR scan needs this HTTPS URL)"
exec "${ROOT}/scripts/run_web_master.sh"
