#!/usr/bin/env bash
set -euo pipefail

SERVER_BIN="${1:?server bin path required}"
CLIENT_BIN="${2:?client bin path required}"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "missing required command: $1" >&2; exit 127; }
}

need_cmd openssl
need_cmd mktemp
need_cmd kill
need_cmd python3
need_cmd grep

TMP_DIR="$(mktemp -d)"
cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  rm -rf "${TMP_DIR}" || true
}
trap cleanup EXIT

CERT_PEM="${TMP_DIR}/cert.pem"
KEY_PEM="${TMP_DIR}/key.pem"

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "${KEY_PEM}" -out "${CERT_PEM}" -days 1 -subj "/CN=localhost" \
  >/dev/null 2>&1

PORT="$(python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 0))
print(s.getsockname()[1])
s.close()
PY
)"

export PORT

"${SERVER_BIN}" "${PORT}" "${CERT_PEM}" "${KEY_PEM}" >/dev/null 2>&1 &
SERVER_PID=$!

# Wait for server to be ready. UDBS pings aren't reliable for QUIC listeners.
sleep 0.5

OUT="$(${CLIENT_BIN} 127.0.0.1 "${PORT}" '{"model":"demo","prompt":"hi"}' 2>/dev/null)"

echo "${OUT}" | grep -q "MDP demo"
echo "${OUT}" | grep -q "Hello"
