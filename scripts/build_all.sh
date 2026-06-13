#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is not set. Source ESP-IDF export.sh first." >&2
  exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

build_one() {
  local target="$1"
  echo "=== Building ${target} ==="
  (
    cd "${ROOT}/firmware/${target}"
    idf.py set-target esp32c6
    idf.py build
  )
}

build_one coordinator
build_one shelf_node

echo "Build complete."
