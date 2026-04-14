#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd -- "${PROJECT_DIR}/.." && pwd)"
SERVER_SCRIPT="${PROJECT_DIR}/scripts/device_server.py"
RUN_DIR="${DEVICE_SERVER_RUN_DIR:-${PROJECT_DIR}/.run}"

HOST="${DEVICE_SERVER_HOST:-${TOKEN_SERVER_HOST:-0.0.0.0}}"
PORT="${DEVICE_SERVER_PORT:-${TOKEN_SERVER_PORT:-8790}}"
TOKEN_PATH="${DEVICE_SERVER_LEGACY_TOKEN_PATH:-${TOKEN_SERVER_HTTP_PATH:-/token}}"
TTL_SECONDS="${DEVICE_SERVER_TTL_SECONDS:-${TOKEN_SERVER_TTL_SECONDS:-3600}}"
ENV_FILE="${DEVICE_SERVER_ENV_FILE:-${TOKEN_SERVER_ENV_FILE:-${PROJECT_DIR}/configs/device_server.local.env}}"
AUTH_PATH="${DEVICE_SERVER_AUTH_PATH:-/v1/auth/token}"
EVENT_PATH="${DEVICE_SERVER_EVENT_PATH:-/v1/diagnostics/events}"
BLOB_PATH="${DEVICE_SERVER_BLOB_PATH:-/v1/diagnostics/blobs}"
ADMIN_PATH="${DEVICE_SERVER_ADMIN_PATH:-/v1/admin/storage}"
DATA_DIR="${DEVICE_SERVER_DATA_DIR:-${PROJECT_DIR}/.run/device_server}"
MAX_EVENT_BYTES="${DEVICE_SERVER_MAX_EVENT_BYTES:-65536}"
MAX_BLOB_BYTES="${DEVICE_SERVER_MAX_BLOB_BYTES:-1048576}"

if [[ "${ENV_FILE}" != /* ]]; then
    ENV_FILE="${PROJECT_DIR}/${ENV_FILE}"
fi

if [[ -x "${REPO_DIR}/.venv/bin/python3" ]]; then
    PYTHON_BIN="${REPO_DIR}/.venv/bin/python3"
elif [[ -x "${PROJECT_DIR}/.venv/bin/python3" ]]; then
    PYTHON_BIN="${PROJECT_DIR}/.venv/bin/python3"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python3)"
else
    echo "python3 is not available" >&2
    exit 1
fi

if [[ ! -f "${ENV_FILE}" ]]; then
    echo "missing env file: ${ENV_FILE}" >&2
    exit 1
fi

mkdir -p "${RUN_DIR}"
cd "${PROJECT_DIR}"

exec env PYTHONUNBUFFERED=1 "${PYTHON_BIN}" "${SERVER_SCRIPT}" \
    --host "${HOST}" \
    --port "${PORT}" \
    --path "${TOKEN_PATH}" \
    --auth-path "${AUTH_PATH}" \
    --event-path "${EVENT_PATH}" \
    --blob-path "${BLOB_PATH}" \
    --admin-path "${ADMIN_PATH}" \
    --env-file "${ENV_FILE}" \
    --ttl-seconds "${TTL_SECONDS}" \
    --data-dir "${DATA_DIR}" \
    --max-event-bytes "${MAX_EVENT_BYTES}" \
    --max-blob-bytes "${MAX_BLOB_BYTES}"
