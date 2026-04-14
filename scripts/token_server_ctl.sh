#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

export TOKEN_SERVER_ENV_FILE="${TOKEN_SERVER_ENV_FILE:-${PROJECT_DIR}/configs/token_server.local.env}"
export DEVICE_SERVER_DATA_DIR="${DEVICE_SERVER_DATA_DIR:-${PROJECT_DIR}/.run/device_server}"
export DEVICE_SERVER_RUN_DIR="${DEVICE_SERVER_RUN_DIR:-${PROJECT_DIR}/.run}"
export DEVICE_SERVER_PID_FILE="${DEVICE_SERVER_PID_FILE:-${PROJECT_DIR}/.run/token_server.pid}"
export DEVICE_SERVER_LOG_FILE="${DEVICE_SERVER_LOG_FILE:-${PROJECT_DIR}/.run/token_server.log}"

exec "${PROJECT_DIR}/device_server/scripts/device_server_ctl.sh" "$@"
