#!/usr/bin/env bash
# Native Zigbee2MQTT stack on macOS (USB dongle cannot pass through Docker Desktop).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="${ROOT}/gateway/zigbee2mqtt"
RUNTIME="${ROOT}/gateway/zigbee2mqtt-runtime"
MOSQUITTO_CONF="${ROOT}/gateway/mosquitto.conf"

DONGLE="$(python3 -c "import sys; sys.path.insert(0, '${ROOT}/gateway'); from dongle import discover_zigbee_dongle_port; print(discover_zigbee_dongle_port() or '')")"
if [[ -z "${DONGLE}" ]]; then
  echo "SONOFF dongle not found. Plug in ZBDongle-E and retry." >&2
  exit 1
fi

python3 - <<PY
from pathlib import Path
import re
cfg = Path("${DATA}/configuration.yaml")
text = cfg.read_text()
text = re.sub(r"^  port: .*", f"  port: ${DONGLE}", text, count=1, flags=re.M)
if "adapter: ember" not in text:
    text = re.sub(r"^  adapter: .*", "  adapter: ember", text, count=1, flags=re.M)
if "rtscts:" not in text:
    text = re.sub(r"^  adapter: ember", "  adapter: ember\n  rtscts: false", text, count=1, flags=re.M)
cfg.write_text(text)
print(f"Dongle port: ${DONGLE}")
PY

if [[ ! -d "${RUNTIME}/node_modules" ]]; then
  git clone --depth 1 https://github.com/Koenkk/zigbee2mqtt.git "${RUNTIME}"
  (cd "${RUNTIME}" && npm install)
fi

if ! lsof -i :1883 >/dev/null 2>&1; then
  echo "Starting Mosquitto on port 1883..."
  /opt/homebrew/opt/mosquitto/sbin/mosquitto -c "${MOSQUITTO_CONF}" &
  sleep 1
fi

echo "Starting Zigbee2MQTT (frontend http://localhost:8081)..."
export ZIGBEE2MQTT_DATA="${DATA}"
cd "${RUNTIME}" && npm start
