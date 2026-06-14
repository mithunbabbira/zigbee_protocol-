#!/usr/bin/env bash
# Flash shelf_node as Zigbee router (one unit) or end device (default).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROLE="${1:-end_device}"
PORT="${2:-}"

if [[ "${ROLE}" != "router" && "${ROLE}" != "end_device" ]]; then
  echo "Usage: $0 [router|end_device] [serial-port]" >&2
  exit 1
fi

source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" 2>/dev/null || source "$HOME/.espressif/esp-idf/export.sh"

cd "${ROOT}/firmware/shelf_node"
cat "sdkconfig.defaults.common" "sdkconfig.defaults.${ROLE}" > sdkconfig.defaults
idf.py set-target esp32c6
idf.py fullclean build

if [[ -n "${PORT}" ]]; then
  idf.py -p "${PORT}" erase-flash flash
  echo ""
  echo "Reading Zigbee IEEE (EUI-64) from flashed board..."
  MAC_LINE="$(python -m esptool --chip esp32c6 -p "${PORT}" read_mac 2>/dev/null | awk '/MAC:/ && !/BASE/ {print $2}')"
  if [[ -n "${MAC_LINE}" ]]; then
    # esptool MAC is aa:bb:cc:dd:ee:ff:gg:hh -> IEEE 0xaabbccddeeffgghh with ff:fe inserted per Espressif
    IEEE="$(python3 - <<PY
mac = "${MAC_LINE}".lower().split(":")
if len(mac) == 8:
    print(f"0x{''.join(mac)}")
elif len(mac) == 6:
    print(f"0x{''.join(mac[:3])}fffe{''.join(mac[3:])}")
PY
)"
    echo "Board IEEE address: ${IEEE}"
    echo "After erase-flash, remove stale pairing in Zigbee2MQTT then permit join:"
    echo "  ${ROOT}/scripts/remove_z2m_device.sh ${IEEE}"
    echo "  mosquitto_pub -h 127.0.0.1 -t zigbee2mqtt/bridge/request/permit_join -m '{\"time\":180}'"
    echo "Place the board within ~2 m of the SONOFF dongle and press RESET."
  fi
else
  echo "Built shelf_node (${ROLE}). Flash with: $0 ${ROLE} /dev/cu.usbmodemXXXX"
fi
