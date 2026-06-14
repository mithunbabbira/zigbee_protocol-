#!/usr/bin/env bash
# Self-signed TLS cert for mobile camera (getUserMedia requires HTTPS off localhost).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CERT_DIR="${ROOT}/gateway/web_app/certs"
KEY="${CERT_DIR}/dev-key.pem"
CERT="${CERT_DIR}/dev-cert.pem"

if [[ -f "${KEY}" && -f "${CERT}" ]]; then
  echo "Dev TLS cert already exists: ${CERT_DIR}"
  exit 0
fi

mkdir -p "${CERT_DIR}"

LAN_IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)"
HOSTNAME="$(scutil --get LocalHostName 2>/dev/null || hostname -s)"
SAN="DNS:localhost,DNS:${HOSTNAME}.local"
if [[ -n "${LAN_IP}" ]]; then
  SAN="${SAN},IP:127.0.0.1,IP:${LAN_IP}"
else
  SAN="${SAN},IP:127.0.0.1"
fi

openssl req -x509 -newkey rsa:2048 \
  -keyout "${KEY}" -out "${CERT}" \
  -days 825 -nodes \
  -subj "/CN=shelf-master.local/O=Shelf Dev" \
  -addext "subjectAltName=${SAN}" 2>/dev/null

chmod 600 "${KEY}"
echo "Created dev TLS cert in ${CERT_DIR}"
if [[ -n "${LAN_IP}" ]]; then
  echo "On your phone use: https://${LAN_IP}:8080"
  echo "Accept the certificate warning once (Advanced → Proceed)."
fi
