#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WEB_PORT="${2:-8080}"
BACKEND="${SHELF_BACKEND:-mqtt}"

python3 -m pip install -q -r "${ROOT}/gateway/requirements.txt"

ARGS=(--backend "${BACKEND}" --web-port "${WEB_PORT}")
if [[ -n "${1:-}" ]]; then
  if [[ "${BACKEND}" == "serial" ]]; then
    ARGS=(--backend serial --port "${1}" --web-port "${WEB_PORT}")
  fi
fi

exec python3 "${ROOT}/gateway/web_app/app.py" "${ARGS[@]}"
