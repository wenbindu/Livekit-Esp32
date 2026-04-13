#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SERVER_SCRIPT="${PROJECT_DIR}/scripts/debug_uplink_ws_server.py"
PID_DIR="${PROJECT_DIR}/.run"
PID_FILE="${PID_DIR}/debug_audio_ws.pid"
LOG_FILE="${PID_DIR}/debug_audio_ws.log"
OUTPUT_DIR="${PROJECT_DIR}/debug_audio_ws"

HOST="${DEBUG_AUDIO_WS_HOST:-0.0.0.0}"
PORT="${DEBUG_AUDIO_WS_PORT:-8765}"
IDLE_SECONDS="${DEBUG_AUDIO_WS_IDLE_SECONDS:-20}"

if [[ -x "${PROJECT_DIR}/.venv/bin/python3" ]]; then
    PYTHON_BIN="${PROJECT_DIR}/.venv/bin/python3"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python3)"
else
    echo "python3 is not available" >&2
    exit 1
fi

mkdir -p "${PID_DIR}" "${OUTPUT_DIR}"

if [[ -f "${PID_FILE}" ]]; then
    existing_pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
    if [[ -n "${existing_pid}" ]] && kill -0 "${existing_pid}" 2>/dev/null; then
        echo "debug audio ws server already running: pid=${existing_pid}"
        echo "log: ${LOG_FILE}"
        echo "output: ${OUTPUT_DIR}"
        exit 0
    fi
    rm -f "${PID_FILE}"
fi

if command -v lsof >/dev/null 2>&1; then
    if lsof -nP -iTCP:"${PORT}" -sTCP:LISTEN >/dev/null 2>&1; then
        echo "port ${PORT} is already in use" >&2
        lsof -nP -iTCP:"${PORT}" -sTCP:LISTEN >&2 || true
        exit 1
    fi
fi

cd "${PROJECT_DIR}"
nohup "${PYTHON_BIN}" "${SERVER_SCRIPT}" \
    --host "${HOST}" \
    --port "${PORT}" \
    --path /uplink \
    --path /downlink \
    --output-dir "${OUTPUT_DIR}" \
    --session-idle-seconds "${IDLE_SECONDS}" \
    >"${LOG_FILE}" 2>&1 &

server_pid="$!"
echo "${server_pid}" > "${PID_FILE}"

for _ in $(seq 1 20); do
    if ! kill -0 "${server_pid}" 2>/dev/null; then
        echo "failed to start debug audio ws server" >&2
        [[ -f "${LOG_FILE}" ]] && tail -n 40 "${LOG_FILE}" >&2
        rm -f "${PID_FILE}"
        exit 1
    fi
    if command -v lsof >/dev/null 2>&1; then
        if lsof -nP -a -p "${server_pid}" -iTCP:"${PORT}" -sTCP:LISTEN >/dev/null 2>&1; then
            echo "debug audio ws server started"
            echo "pid: ${server_pid}"
            echo "log: ${LOG_FILE}"
            echo "output: ${OUTPUT_DIR}"
            exit 0
        fi
    else
        sleep 1
        echo "debug audio ws server started"
        echo "pid: ${server_pid}"
        echo "log: ${LOG_FILE}"
        echo "output: ${OUTPUT_DIR}"
        exit 0
    fi
    sleep 0.25
done

echo "debug audio ws server did not bind to port ${PORT}" >&2
[[ -f "${LOG_FILE}" ]] && tail -n 40 "${LOG_FILE}" >&2
rm -f "${PID_FILE}"
exit 1
