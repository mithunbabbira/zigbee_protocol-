#!/usr/bin/env bash
# Flash shelf_node to ESP32-C6 (sources ESP-IDF automatically).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROLE="${1:-end_device}"
PORT="${2:-}"

if [[ "${ROLE}" != "router" && "${ROLE}" != "end_device" ]]; then
  echo "Usage: $0 [router|end_device] [serial-port]" >&2
  exit 1
fi

source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" 2>/dev/null || source "$HOME/.espressif/esp-idf/export.sh"

exec "${ROOT}/scripts/flash_shelf.sh" "${ROLE}" ${PORT:+"${PORT}"}
