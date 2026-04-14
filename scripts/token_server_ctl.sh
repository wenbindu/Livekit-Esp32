#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SERVER_SCRIPT="${PROJECT_DIR}/scripts/token_server.py"
RUN_DIR="${PROJECT_DIR}/.run"
PID_FILE="${RUN_DIR}/token_server.pid"
LOG_FILE="${RUN_DIR}/token_server.log"

HOST="${TOKEN_SERVER_HOST:-0.0.0.0}"
PORT="${TOKEN_SERVER_PORT:-8790}"
TOKEN_PATH="${TOKEN_SERVER_HTTP_PATH:-/token}"
TTL_SECONDS="${TOKEN_SERVER_TTL_SECONDS:-3600}"
ENV_FILE="${TOKEN_SERVER_ENV_FILE:-${PROJECT_DIR}/configs/token_server.local.env}"
DISPLAY_HOST="${TOKEN_SERVER_PUBLIC_HOST:-${HOST}}"
AUTH_PATH="${DEVICE_SERVER_AUTH_PATH:-/v1/auth/token}"
EVENT_PATH="${DEVICE_SERVER_EVENT_PATH:-/v1/diagnostics/events}"
BLOB_PATH="${DEVICE_SERVER_BLOB_PATH:-/v1/diagnostics/blobs}"
ADMIN_PATH="${DEVICE_SERVER_ADMIN_PATH:-/v1/admin/storage}"
DATA_DIR="${DEVICE_SERVER_DATA_DIR:-${PROJECT_DIR}/.run/device_server}"
MAX_EVENT_BYTES="${DEVICE_SERVER_MAX_EVENT_BYTES:-65536}"
MAX_BLOB_BYTES="${DEVICE_SERVER_MAX_BLOB_BYTES:-1048576}"

usage() {
    cat <<'EOF'
Usage:
  bash scripts/token_server_ctl.sh start
  bash scripts/token_server_ctl.sh stop
  bash scripts/token_server_ctl.sh status
  bash scripts/token_server_ctl.sh restart
  bash scripts/token_server_ctl.sh logs

Environment:
  TOKEN_SERVER_HOST        Bind host, default: 0.0.0.0
  TOKEN_SERVER_PORT        Bind port, default: 8790
  TOKEN_SERVER_HTTP_PATH   Legacy token path, default: /token
  TOKEN_SERVER_TTL_SECONDS JWT TTL, default: 3600
  TOKEN_SERVER_ENV_FILE    Env file, default: configs/token_server.local.env
  TOKEN_SERVER_PUBLIC_HOST Optional display host for printed URLs
  DEVICE_SERVER_AUTH_PATH  Auth API path, default: /v1/auth/token
  DEVICE_SERVER_EVENT_PATH Diagnostics event ingest path
  DEVICE_SERVER_BLOB_PATH  Diagnostics blob ingest path
  DEVICE_SERVER_ADMIN_PATH Admin summary path
  DEVICE_SERVER_DATA_DIR   Diagnostics storage root
  DEVICE_SERVER_MAX_EVENT_BYTES
  DEVICE_SERVER_MAX_BLOB_BYTES
EOF
}

if [[ "${ENV_FILE}" != /* ]]; then
    ENV_FILE="${PROJECT_DIR}/${ENV_FILE}"
fi

if [[ "${DISPLAY_HOST}" == "0.0.0.0" ]]; then
    DISPLAY_HOST="127.0.0.1"
fi

if [[ -x "${PROJECT_DIR}/.venv/bin/python3" ]]; then
    PYTHON_BIN="${PROJECT_DIR}/.venv/bin/python3"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python3)"
else
    echo "python3 is not available" >&2
    exit 1
fi

mkdir -p "${RUN_DIR}"

pid_is_running() {
    local pid="$1"
    [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null
}

add_unique_pid() {
    local pid="$1"
    local existing
    [[ -z "${pid}" ]] && return 0
    for existing in "${PIDS[@]:-}"; do
        if [[ "${existing}" == "${pid}" ]]; then
            return 0
        fi
    done
    PIDS+=("${pid}")
}

collect_pids() {
    PIDS=()

    if [[ -f "${PID_FILE}" ]]; then
        local pid
        pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
        if pid_is_running "${pid}"; then
            add_unique_pid "${pid}"
        else
            rm -f "${PID_FILE}"
        fi
    fi

    if command -v pgrep >/dev/null 2>&1; then
        local pid
        while IFS= read -r pid; do
            pid_is_running "${pid}" || continue
            add_unique_pid "${pid}"
        done < <(pgrep -f "${SERVER_SCRIPT}" 2>/dev/null || true)
    fi
}

print_endpoints() {
    echo "bind: ${HOST}:${PORT}"
    echo "env: ${ENV_FILE}"
    echo "log: ${LOG_FILE}"
    echo "data: ${DATA_DIR}"
    echo "health: http://${DISPLAY_HOST}:${PORT}/healthz"
    echo "token legacy: http://${DISPLAY_HOST}:${PORT}${TOKEN_PATH}"
    echo "token auth:   http://${DISPLAY_HOST}:${PORT}${AUTH_PATH}"
    echo "diag events:  http://${DISPLAY_HOST}:${PORT}${EVENT_PATH}"
    echo "diag blobs:   http://${DISPLAY_HOST}:${PORT}${BLOB_PATH}"
    echo "admin:        http://${DISPLAY_HOST}:${PORT}${ADMIN_PATH}"
}

start_server() {
    if [[ ! -f "${ENV_FILE}" ]]; then
        echo "missing env file: ${ENV_FILE}" >&2
        return 1
    fi

    collect_pids
    if [[ "${#PIDS[@]}" -gt 0 ]]; then
        echo "token server already running: pid=${PIDS[0]}"
        print_endpoints
        return 0
    fi

    if command -v lsof >/dev/null 2>&1; then
        if lsof -nP -iTCP:"${PORT}" -sTCP:LISTEN >/dev/null 2>&1; then
            echo "port ${PORT} is already in use" >&2
            lsof -nP -iTCP:"${PORT}" -sTCP:LISTEN >&2 || true
            return 1
        fi
    fi

    cd "${PROJECT_DIR}"
    PYTHONUNBUFFERED=1 nohup "${PYTHON_BIN}" "${SERVER_SCRIPT}" \
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
        --max-blob-bytes "${MAX_BLOB_BYTES}" \
        >"${LOG_FILE}" 2>&1 &

    local server_pid="$!"
    echo "${server_pid}" > "${PID_FILE}"

    local _i
    for _i in $(seq 1 20); do
        if ! pid_is_running "${server_pid}"; then
            echo "failed to start token server" >&2
            [[ -f "${LOG_FILE}" ]] && tail -n 40 "${LOG_FILE}" >&2
            rm -f "${PID_FILE}"
            return 1
        fi

        if command -v lsof >/dev/null 2>&1; then
            if lsof -nP -a -p "${server_pid}" -iTCP:"${PORT}" -sTCP:LISTEN >/dev/null 2>&1; then
                echo "token server started: pid=${server_pid}"
                print_endpoints
                return 0
            fi
        else
            sleep 1
            echo "token server started: pid=${server_pid}"
            print_endpoints
            return 0
        fi

        sleep 0.25
    done

    echo "token server did not bind to port ${PORT}" >&2
    [[ -f "${LOG_FILE}" ]] && tail -n 40 "${LOG_FILE}" >&2
    rm -f "${PID_FILE}"
    return 1
}

stop_server() {
    collect_pids
    if [[ "${#PIDS[@]}" -eq 0 ]]; then
        rm -f "${PID_FILE}"
        echo "token server is not running"
        return 0
    fi

    kill "${PIDS[@]}" 2>/dev/null || true

    local _i
    for _i in $(seq 1 20); do
        local still_running=0
        local pid
        for pid in "${PIDS[@]}"; do
            if pid_is_running "${pid}"; then
                still_running=1
                break
            fi
        done

        if [[ "${still_running}" -eq 0 ]]; then
            rm -f "${PID_FILE}"
            echo "token server stopped"
            return 0
        fi
        sleep 0.25
    done

    kill -9 "${PIDS[@]}" 2>/dev/null || true
    rm -f "${PID_FILE}"
    echo "token server force stopped"
    return 0
}

status_server() {
    collect_pids
    if [[ "${#PIDS[@]}" -eq 0 ]]; then
        echo "token server is not running"
        print_endpoints
        return 1
    fi

    echo "token server is running: pid=${PIDS[0]}"
    print_endpoints
    return 0
}

logs_server() {
    if [[ ! -f "${LOG_FILE}" ]]; then
        echo "log file does not exist: ${LOG_FILE}" >&2
        return 1
    fi

    tail -n "${TOKEN_SERVER_LOG_LINES:-80}" -f "${LOG_FILE}"
}

COMMAND="${1:-start}"

case "${COMMAND}" in
    start)
        start_server
        ;;
    stop)
        stop_server
        ;;
    status)
        status_server
        ;;
    restart)
        stop_server && start_server
        ;;
    logs)
        logs_server
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
