#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SERVER_SCRIPT="${PROJECT_DIR}/scripts/debug_uplink_ws_server.py"
PID_FILE="${PROJECT_DIR}/.run/debug_audio_ws.pid"

declare -a pids=()

if [[ -f "${PID_FILE}" ]]; then
    pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        pids+=("${pid}")
    fi
fi

if command -v pgrep >/dev/null 2>&1; then
    while IFS= read -r pid; do
        [[ -z "${pid}" ]] && continue
        already_listed=0
        for existing in "${pids[@]:-}"; do
            if [[ "${existing}" == "${pid}" ]]; then
                already_listed=1
                break
            fi
        done
        if [[ "${already_listed}" -eq 0 ]]; then
            pids+=("${pid}")
        fi
    done < <(pgrep -f "${SERVER_SCRIPT}" 2>/dev/null || true)
fi

if [[ "${#pids[@]}" -eq 0 ]]; then
    rm -f "${PID_FILE}"
    echo "debug audio ws server is not running"
    exit 0
fi

kill "${pids[@]}" 2>/dev/null || true

for _ in $(seq 1 20); do
    still_running=0
    for pid in "${pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            still_running=1
            break
        fi
    done
    if [[ "${still_running}" -eq 0 ]]; then
        rm -f "${PID_FILE}"
        echo "debug audio ws server stopped"
        exit 0
    fi
    sleep 0.25
done

kill -9 "${pids[@]}" 2>/dev/null || true
rm -f "${PID_FILE}"
echo "debug audio ws server force stopped"
