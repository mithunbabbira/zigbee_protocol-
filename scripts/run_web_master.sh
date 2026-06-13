#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WEB_PORT="${2:-8080}"

python3 -m pip install -q -r "${ROOT}/gateway/requirements.txt"
if [[ -n "${1:-}" ]]; then
  exec python3 "${ROOT}/gateway/web_app/app.py" --port "${1}" --web-port "${WEB_PORT}"
fi
exec python3 "${ROOT}/gateway/web_app/app.py" --web-port "${WEB_PORT}"
