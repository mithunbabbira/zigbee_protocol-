#!/usr/bin/env bash
# Flash ESP32-C6 coordinator firmware.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-}"

source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" 2>/dev/null || source "$HOME/.espressif/esp-idf/export.sh"

cd "${ROOT}/firmware/coordinator"
idf.py set-target esp32c6
idf.py build

if [[ -n "${PORT}" ]]; then
  idf.py -p "${PORT}" erase-flash flash monitor
else
  echo "Built coordinator. Flash with: $0 /dev/cu.usbmodemXXXX"
fi
