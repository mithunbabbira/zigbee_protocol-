#!/usr/bin/env bash
# Run Zigbee2MQTT + Mosquitto via Docker for the SONOFF ZBDongle-E.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="${ROOT}/gateway/zigbee2mqtt"
CONFIG="${DATA}/configuration.yaml"

DONGLE="$(python3 -c "import sys; sys.path.insert(0, '${ROOT}/gateway'); from dongle import discover_zigbee_dongle_port; print(discover_zigbee_dongle_port() or '')")"
if [[ -z "${DONGLE}" ]]; then
  echo "Plug in the SONOFF ZBDongle-E and retry." >&2
  echo "Expected Silicon Labs CP210x on /dev/cu.usbserial-*" >&2
  exit 1
fi

python3 - <<PY
from pathlib import Path
import re
cfg = Path("${CONFIG}")
text = cfg.read_text()
text = re.sub(r"^  port: .*", f"  port: ${DONGLE}", text, count=1, flags=re.M)
cfg.write_text(text)
print(f"Updated {cfg} serial port -> ${DONGLE}")
PY

docker network inspect shelf-zigbee >/dev/null 2>&1 || docker network create shelf-zigbee

docker rm -f shelf-mosquitto shelf-zigbee2mqtt 2>/dev/null || true

docker run -d --name shelf-mosquitto --network shelf-zigbee -p 1883:1883 eclipse-mosquitto:2

docker run -d --name shelf-zigbee2mqtt --network shelf-zigbee \
  --device="${DONGLE}" \
  -v "${DATA}:/app/data" \
  -p 8081:8080 \
  -e TZ=UTC \
  koenkk/zigbee2mqtt

echo "Zigbee2MQTT frontend: http://localhost:8081"
echo "MQTT broker: mqtt://127.0.0.1:1883"
echo "Then run: ./scripts/run_web_master.sh"
