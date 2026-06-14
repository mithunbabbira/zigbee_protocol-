#!/usr/bin/env bash
# Start Put-to-Light stack: web UI on serial JSON backend (ESP32-C6 coordinator).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-}"

if [[ -z "${PORT}" ]]; then
  for candidate in /dev/cu.usbmodem*; do
    [[ -e "${candidate}" ]] || continue
    PORT="${candidate}"
    break
  done
fi

if [[ -z "${PORT}" ]]; then
  echo "Plug in ESP32-C6 coordinator USB and retry, or: $0 /dev/cu.usbmodemXXXX" >&2
  exit 1
fi

echo "Coordinator port: ${PORT}"
echo "Web UI: http://localhost:8080"
echo "Do NOT run Zigbee2MQTT at the same time on this port."

exec "${ROOT}/scripts/run_web_master.sh" "${PORT}"
