#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_TARGET="esp32s3"
DEFAULT_BOARD="lichuang_esp32s3"
DEFAULT_PROFILE="dev"
GENERATED_DEFAULTS="sdkconfig.defaults.generated"
CONFIG_STAMP_FILE=".project_config.stamp"
DEFAULT_ENV_FILE="${PROJECT_DIR}/configs/livekit.local.env"
DEFAULT_BRANCH_ENV_FILE="${PROJECT_DIR}/configs/branch.defaults.env"
DEFAULT_PRESET_DIR="${PROJECT_DIR}/configs/presets"

usage() {
    cat <<'EOF'
Usage: scripts/project.sh <command>

Commands:
  configure      Regenerate sdkconfig from board/profile/local config
  port           Print the detected serial port
  build          Build firmware
  clean          Run idf.py clean
  fullclean      Run idf.py fullclean
  menuconfig     Open menuconfig
  flash          Flash to the detected serial port
  monitor        Open serial monitor on the detected serial port
  flash-monitor  Flash and then open the serial monitor

Environment:
  BOARD               Board config name, default: lichuang_esp32s3
  PROFILE             Build profile name, default: dev
  CONFIG_ENV_FILE     Path to local env file, default: configs/livekit.local.env
  FIRMWARE_PRESET     Optional diagnostic preset from configs/presets/<name>.env
  ESPPORT             Override auto-detected serial port

Branch Defaults:
  configs/branch.defaults.env is auto-loaded after the local env file.
  Lifecycle branches own only their baseline behavior.

Preset Overlays:
  When FIRMWARE_PRESET is set, configs/presets/<name>.env is loaded after the
  branch defaults so temporary diagnostics can override branch behavior.

Config env keys:
  AUTH_MODE=device_jwt|token_server|sandbox|static_token
  TOKEN_SERVER_URL=http://host:8790/token
  TOKEN_SERVER_TIMEOUT_MS=5000
  TOKEN_SERVER_RETRY_DELAY_MS=3000
  TOKEN_SERVER_AUTH_MAX_FAILURES=3
  LIVEKIT_SANDBOX_ID=bo-xxxxxx
  LIVEKIT_URL=wss://project.livekit.cloud
  LIVEKIT_API_KEY=API...
  LIVEKIT_API_SECRET=secret...
  LIVEKIT_JWT_TTL_SECONDS=3600
  LIVEKIT_TOKEN=eyJ...
  LIVEKIT_ROOM=lichuang-room
  LIVEKIT_PARTICIPANT=lichuang-esp32s3
  LIVEKIT_PARTICIPANT_IDENTITY=lichuang-esp32s3
  LIVEKIT_PARTICIPANT_METADATA={...}
  LIVEKIT_AGENT_NAME=my-agent
  LIVEKIT_AGENT_METADATA={...}
  ENABLE_DEBUG_UPLINK_WS=0|1
  ENABLE_DEBUG_DOWNLINK_WS=0|1
  DEBUG_UPLINK_WS_URL=ws://host:8765/uplink
  DEBUG_DOWNLINK_WS_URL=ws://host:8765/downlink
  DEBUG_UPLINK_WS_RECONNECT_MS=3000
  LOCAL_AUDIO_UPLINK_ONLY=0|1
  ENABLE_DEBUG_UPLINK_WAV=0|1
  DEBUG_UPLINK_RECORD_SECONDS=30
  DEBUG_HTTP_PORT=8080
  START_IN_STANDBY=0|1
EOF
}

ensure_idf_env() {
    if command -v idf.py >/dev/null 2>&1; then
        return 0
    fi

    local candidates=()
    if [[ -n "${IDF_PATH:-}" ]]; then
        candidates+=("${IDF_PATH}/export.sh")
    fi
    candidates+=(
        "${PROJECT_DIR}/../esp-idf/export.sh"
        "${HOME}/esp/esp-idf/export.sh"
    )

    local export_sh
    for export_sh in "${candidates[@]}"; do
        if [[ -f "${export_sh}" ]]; then
            # shellcheck disable=SC1090
            . "${export_sh}" >/dev/null
            if command -v idf.py >/dev/null 2>&1; then
                return 0
            fi
        fi
    done

    echo "idf.py is not available. Source ESP-IDF export.sh first." >&2
    return 1
}

load_optional_env() {
    local env_file="$1"
    if [[ -z "${env_file}" || ! -f "${env_file}" ]]; then
        return 0
    fi

    set -a
    # shellcheck disable=SC1090
    . "${env_file}"
    set +a
}

resolve_preset_env_file() {
    local preset_name="${FIRMWARE_PRESET:-}"
    if [[ -z "${preset_name}" ]]; then
        return 0
    fi

    local preset_file="${DEFAULT_PRESET_DIR}/${preset_name}.env"
    if [[ ! -f "${preset_file}" ]]; then
        echo "Unknown FIRMWARE_PRESET=${preset_name}. Expected ${preset_file}" >&2
        return 1
    fi

    printf '%s\n' "${preset_file}"
}

load_config_env() {
    local env_file="${CONFIG_ENV_FILE:-${DEFAULT_ENV_FILE}}"
    load_optional_env "${env_file}"
    load_optional_env "${DEFAULT_BRANCH_ENV_FILE}"

    local preset_file=""
    preset_file="$(resolve_preset_env_file)"
    if [[ -n "${preset_file}" ]]; then
        load_optional_env "${preset_file}"
    fi
}

detect_port() {
    if [[ -n "${ESPPORT:-}" ]]; then
        echo "${ESPPORT}"
        return 0
    fi

    local patterns=(
        /dev/cu.usbmodem*
        /dev/tty.usbmodem*
        /dev/ttyACM*
        /dev/ttyUSB*
    )
    local matches=()
    local pattern
    local device

    for pattern in "${patterns[@]}"; do
        for device in ${pattern}; do
            if [[ -e "${device}" ]]; then
                matches+=("${device}")
            fi
        done
    done

    if [[ "${#matches[@]}" -eq 0 ]]; then
        echo "No ESP serial device detected. Set ESPPORT=/dev/..." >&2
        return 1
    fi

    if [[ "${#matches[@]}" -gt 1 ]]; then
        printf 'Multiple serial devices detected, defaulting to %s\n' "${matches[0]}" >&2
        printf 'Set ESPPORT to override:\n' >&2
        printf '  %s\n' "${matches[@]}" >&2
    fi

    echo "${matches[0]}"
}

sdkconfig_defaults_arg() {
    local board="${BOARD:-${DEFAULT_BOARD}}"
    local profile="${PROFILE:-${DEFAULT_PROFILE}}"
    local defaults=(
        "configs/sdkconfig.defaults.base"
        "configs/sdkconfig.defaults.board.${board}"
        "configs/sdkconfig.defaults.${profile}"
    )

    if [[ -f "${PROJECT_DIR}/${GENERATED_DEFAULTS}" ]]; then
        defaults+=("${GENERATED_DEFAULTS}")
    fi

    local joined=""
    local file
    for file in "${defaults[@]}"; do
        if [[ -n "${joined}" ]]; then
            joined="${joined};"
        fi
        joined="${joined}${file}"
    done

    printf '%s\n' "${joined}"
}

backup_sdkconfig() {
    local timestamp
    timestamp="$(date +%Y%m%d_%H%M%S)"
    local file="sdkconfig"
    if [[ -f "${PROJECT_DIR}/${file}" ]]; then
        cp "${PROJECT_DIR}/${file}" "${PROJECT_DIR}/${file}.bak.${timestamp}"
        echo "Backed up ${file} -> ${file}.bak.${timestamp}" >&2
    fi
    prune_file_backups "sdkconfig.bak.*" "${SDKCONFIG_BACKUP_KEEP:-3}"
    prune_file_backups "sdkconfig.old.bak.*" 0
}

prune_file_backups() {
    local pattern="$1"
    local keep_count="$2"
    local files=()
    local file

    while IFS= read -r file; do
        [[ -n "${file}" ]] && files+=("${file}")
    done < <(find "${PROJECT_DIR}" -maxdepth 1 -type f -name "${pattern}" | sort)

    if (( ${#files[@]} <= keep_count )); then
        return 0
    fi

    local remove_count=$(( ${#files[@]} - keep_count ))
    local idx
    for (( idx = 0; idx < remove_count; idx++ )); do
        rm -f "${files[idx]}"
    done
}

prepare_build_dir_for_reconfigure() {
    local build_dir="${PROJECT_DIR}/build"
    if [[ ! -d "${build_dir}" ]]; then
        return 0
    fi
    if [[ -f "${build_dir}/CMakeCache.txt" || -f "${build_dir}/build.ninja" ]]; then
        return 0
    fi

    local timestamp
    timestamp="$(date +%Y%m%d_%H%M%S)"
    local backup_dir="${PROJECT_DIR}/build.stale.${timestamp}"
    mv "${build_dir}" "${backup_dir}"
    echo "Moved stale build directory -> $(basename "${backup_dir}")" >&2
}

replace_or_append_config() {
    local file="$1"
    local key="$2"
    local line="$3"

    if grep -Eq "^(# )?${key}(=| is not set)" "${file}"; then
        awk -v key="${key}" -v line="${line}" '
            BEGIN { replaced = 0 }
            $0 ~ ("^(# )?" key "(=| is not set)") {
                if (!replaced) {
                    print line
                    replaced = 1
                }
                next
            }
            { print }
            END {
                if (!replaced) {
                    print line
                }
            }
        ' "${file}" > "${file}.tmp"
        mv "${file}.tmp" "${file}"
    else
        printf '%s\n' "${line}" >> "${file}"
    fi
}

sync_generated_sdkconfig() {
    local sdkconfig_path="${PROJECT_DIR}/sdkconfig"
    local generated_path="${PROJECT_DIR}/${GENERATED_DEFAULTS}"

    [[ -f "${sdkconfig_path}" ]] || return 0
    [[ -f "${generated_path}" ]] || return 0

    while IFS= read -r line || [[ -n "${line}" ]]; do
        [[ -z "${line}" ]] && continue
        [[ "${line}" =~ ^#\  ]] || [[ "${line}" =~ ^CONFIG_ ]] || continue

        local key
        if [[ "${line}" =~ ^#\ (CONFIG_[A-Z0-9_]+)\ is\ not\ set$ ]]; then
            key="${BASH_REMATCH[1]}"
        else
            key="${line%%=*}"
        fi
        replace_or_append_config "${sdkconfig_path}" "${key}" "${line}"
    done < "${generated_path}"

    echo "Synced generated config into sdkconfig" >&2
}

config_stamp_path() {
    printf '%s/%s\n' "${PROJECT_DIR}/build" "${CONFIG_STAMP_FILE}"
}

write_config_stamp() {
    local stamp="$1"
    local stamp_path
    stamp_path="$(config_stamp_path)"
    mkdir -p "$(dirname "${stamp_path}")"
    printf '%s\n' "${stamp}" > "${stamp_path}"
}

current_config_stamp() {
    local board="${BOARD:-${DEFAULT_BOARD}}"
    local profile="${PROFILE:-${DEFAULT_PROFILE}}"
    local board_defaults="${PROJECT_DIR}/configs/sdkconfig.defaults.board.${board}"
    local profile_defaults="${PROJECT_DIR}/configs/sdkconfig.defaults.${profile}"
    local generated_path="${PROJECT_DIR}/${GENERATED_DEFAULTS}"

    {
        printf 'board=%s\n' "${board}"
        printf 'profile=%s\n' "${profile}"
        printf 'preset=%s\n' "${FIRMWARE_PRESET:-}"
        for file in \
            "${PROJECT_DIR}/configs/sdkconfig.defaults.base" \
            "${board_defaults}" \
            "${profile_defaults}" \
            "${generated_path}"; do
            if [[ -f "${file}" ]]; then
                printf 'file=%s\n' "$(basename "${file}")"
                shasum -a 256 "${file}"
            else
                printf 'missing=%s\n' "${file}"
            fi
        done
    } | shasum -a 256 | awk '{print $1}'
}

configure_project() {
    local backup_existing="${1:-1}"

    if [[ "${backup_existing}" == "1" ]]; then
        backup_sdkconfig
    fi

    refresh_generated_defaults
    prepare_build_dir_for_reconfigure
    run_idf_profiled set-target "${DEFAULT_TARGET}"
    sync_generated_sdkconfig
    run_idf reconfigure
    write_config_stamp "$(current_config_stamp)"
}

ensure_project_config() {
    refresh_generated_defaults

    local stamp_path
    stamp_path="$(config_stamp_path)"
    local desired_stamp
    desired_stamp="$(current_config_stamp)"
    local current_stamp=""

    if [[ -f "${stamp_path}" ]]; then
        current_stamp="$(tr -d '\n' < "${stamp_path}")"
    fi

    if [[ ! -f "${PROJECT_DIR}/sdkconfig" || "${current_stamp}" != "${desired_stamp}" ]]; then
        echo "Configuration drift detected, reconfiguring project" >&2
        prepare_build_dir_for_reconfigure
        run_idf_profiled set-target "${DEFAULT_TARGET}"
        sync_generated_sdkconfig
        run_idf reconfigure
        write_config_stamp "${desired_stamp}"
    fi
}

refresh_generated_defaults() {
    local generated_path="${PROJECT_DIR}/${GENERATED_DEFAULTS}"
    : > "${generated_path}"
    printf '# Auto-generated by scripts/project.sh\n' > "${generated_path}"

    local auth_mode="${AUTH_MODE:-token_server}"
    local participant="${LIVEKIT_PARTICIPANT:-lichuang-esp32s3}"
    local identity="${LIVEKIT_PARTICIPANT_IDENTITY:-${participant}}"
    local room="${LIVEKIT_ROOM:-lichuang-room}"
    local uplink_ws_enabled="${ENABLE_DEBUG_UPLINK_WS:-0}"
    local downlink_ws_enabled="${ENABLE_DEBUG_DOWNLINK_WS:-0}"
    local wav_enabled="${ENABLE_DEBUG_UPLINK_WAV:-0}"

    if [[ ("${uplink_ws_enabled}" == "1" || "${downlink_ws_enabled}" == "1") && "${wav_enabled}" == "1" ]]; then
        echo "WebSocket debug audio and WAV debug audio cannot both be enabled" >&2
        return 1
    fi

    printf 'CONFIG_LK_EXAMPLE_ROOM_NAME="%s"\n' "${room}" >> "${generated_path}"
    printf 'CONFIG_LK_EXAMPLE_PARTICIPANT_NAME="%s"\n' "${participant}" >> "${generated_path}"
    printf 'CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY="%s"\n' "${identity}" >> "${generated_path}"

    if [[ -n "${LIVEKIT_PARTICIPANT_METADATA:-}" ]]; then
        printf 'CONFIG_LK_EXAMPLE_PARTICIPANT_METADATA="%s"\n' "${LIVEKIT_PARTICIPANT_METADATA}" >> "${generated_path}"
    fi
    if [[ -n "${LIVEKIT_AGENT_NAME:-}" ]]; then
        printf 'CONFIG_LK_EXAMPLE_AGENT_NAME="%s"\n' "${LIVEKIT_AGENT_NAME}" >> "${generated_path}"
    fi
    if [[ -n "${LIVEKIT_AGENT_METADATA:-}" ]]; then
        printf 'CONFIG_LK_EXAMPLE_AGENT_METADATA="%s"\n' "${LIVEKIT_AGENT_METADATA}" >> "${generated_path}"
    fi

    case "${auth_mode}" in
        device_jwt)
            if [[ -z "${LIVEKIT_URL:-}" || -z "${LIVEKIT_API_KEY:-}" || -z "${LIVEKIT_API_SECRET:-}" ]]; then
                echo "AUTH_MODE=device_jwt requires LIVEKIT_URL, LIVEKIT_API_KEY, and LIVEKIT_API_SECRET" >&2
                return 1
            fi
            printf 'CONFIG_LK_EXAMPLE_USE_DEVICE_JWT=y\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_SANDBOX is not set\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER is not set\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_PREGENERATED is not set\n' >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_SERVER_URL="%s"\n' "${LIVEKIT_URL}" >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_API_KEY="%s"\n' "${LIVEKIT_API_KEY}" >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_API_SECRET="%s"\n' "${LIVEKIT_API_SECRET}" >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_DEVICE_JWT_TTL_SECONDS=%s\n' "${LIVEKIT_JWT_TTL_SECONDS:-3600}" >> "${generated_path}"
            ;;
        token_server)
            if [[ -z "${TOKEN_SERVER_URL:-}" ]]; then
                echo "AUTH_MODE=token_server requires TOKEN_SERVER_URL" >&2
                return 1
            fi
            printf 'CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER=y\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_SANDBOX is not set\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_DEVICE_JWT is not set\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_PREGENERATED is not set\n' >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL="%s"\n' "${TOKEN_SERVER_URL}" >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_TOKEN_SERVER_TIMEOUT_MS=%s\n' "${TOKEN_SERVER_TIMEOUT_MS:-5000}" >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_TOKEN_SERVER_RETRY_DELAY_MS=%s\n' "${TOKEN_SERVER_RETRY_DELAY_MS:-3000}" >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_TOKEN_SERVER_AUTH_MAX_FAILURES=%s\n' "${TOKEN_SERVER_AUTH_MAX_FAILURES:-3}" >> "${generated_path}"
            ;;
        sandbox)
            if [[ -z "${LIVEKIT_SANDBOX_ID:-}" ]]; then
                echo "AUTH_MODE=sandbox requires LIVEKIT_SANDBOX_ID" >&2
                return 1
            fi
            printf 'CONFIG_LK_EXAMPLE_USE_SANDBOX=y\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_DEVICE_JWT is not set\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER is not set\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_PREGENERATED is not set\n' >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_SANDBOX_ID="%s"\n' "${LIVEKIT_SANDBOX_ID}" >> "${generated_path}"
            ;;
        static_token)
            if [[ -z "${LIVEKIT_URL:-}" || -z "${LIVEKIT_TOKEN:-}" ]]; then
                echo "AUTH_MODE=static_token requires LIVEKIT_URL and LIVEKIT_TOKEN" >&2
                return 1
            fi
            printf 'CONFIG_LK_EXAMPLE_USE_PREGENERATED=y\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_SANDBOX is not set\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_DEVICE_JWT is not set\n' >> "${generated_path}"
            printf '# CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER is not set\n' >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_SERVER_URL="%s"\n' "${LIVEKIT_URL}" >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_TOKEN="%s"\n' "${LIVEKIT_TOKEN}" >> "${generated_path}"
            ;;
        *)
            echo "Unsupported AUTH_MODE=${auth_mode}" >&2
            return 1
            ;;
    esac

    if [[ "${uplink_ws_enabled}" == "1" || "${downlink_ws_enabled}" == "1" ]]; then
        if [[ "${uplink_ws_enabled}" == "1" && -z "${DEBUG_UPLINK_WS_URL:-}" ]]; then
            echo "ENABLE_DEBUG_UPLINK_WS=1 requires DEBUG_UPLINK_WS_URL" >&2
            return 1
        fi
        if [[ "${downlink_ws_enabled}" == "1" && -z "${DEBUG_DOWNLINK_WS_URL:-}" ]]; then
            echo "ENABLE_DEBUG_DOWNLINK_WS=1 requires DEBUG_DOWNLINK_WS_URL" >&2
            return 1
        fi
        if [[ "${uplink_ws_enabled}" == "1" ]]; then
            printf 'CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS=y\n' >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS_URL="%s"\n' "${DEBUG_UPLINK_WS_URL}" >> "${generated_path}"
        else
            printf '# CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS is not set\n' >> "${generated_path}"
        fi
        if [[ "${downlink_ws_enabled}" == "1" ]]; then
            printf 'CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS=y\n' >> "${generated_path}"
            printf 'CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS_URL="%s"\n' "${DEBUG_DOWNLINK_WS_URL}" >> "${generated_path}"
        else
            printf '# CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS is not set\n' >> "${generated_path}"
        fi
        printf '# CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WAV is not set\n' >> "${generated_path}"
        printf 'CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS_RECONNECT_MS=%s\n' "${DEBUG_UPLINK_WS_RECONNECT_MS:-3000}" >> "${generated_path}"
        if [[ "${uplink_ws_enabled}" == "1" && "${LOCAL_AUDIO_UPLINK_ONLY:-0}" == "1" ]]; then
            printf 'CONFIG_LK_EXAMPLE_LOCAL_AUDIO_UPLINK_ONLY=y\n' >> "${generated_path}"
        else
            printf '# CONFIG_LK_EXAMPLE_LOCAL_AUDIO_UPLINK_ONLY is not set\n' >> "${generated_path}"
        fi
    elif [[ "${wav_enabled}" == "1" ]]; then
        printf 'CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WAV=y\n' >> "${generated_path}"
        printf '# CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS is not set\n' >> "${generated_path}"
        printf '# CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS is not set\n' >> "${generated_path}"
        printf 'CONFIG_LK_EXAMPLE_DEBUG_UPLINK_RECORD_SECONDS=%s\n' "${DEBUG_UPLINK_RECORD_SECONDS:-30}" >> "${generated_path}"
        printf 'CONFIG_LK_EXAMPLE_DEBUG_HTTP_PORT=%s\n' "${DEBUG_HTTP_PORT:-8080}" >> "${generated_path}"
    else
        printf '# CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WAV is not set\n' >> "${generated_path}"
        printf '# CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS is not set\n' >> "${generated_path}"
        printf '# CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS is not set\n' >> "${generated_path}"
        printf '# CONFIG_LK_EXAMPLE_LOCAL_AUDIO_UPLINK_ONLY is not set\n' >> "${generated_path}"
    fi

    if [[ "${START_IN_STANDBY:-0}" == "1" ]]; then
        printf 'CONFIG_LK_EXAMPLE_START_IN_STANDBY=y\n' >> "${generated_path}"
    fi

    echo "Generated ${GENERATED_DEFAULTS} for board=${BOARD:-${DEFAULT_BOARD}} profile=${PROFILE:-${DEFAULT_PROFILE}}" >&2
}

run_idf_profiled() {
    ensure_idf_env
    idf.py -C "${PROJECT_DIR}" -DSDKCONFIG_DEFAULTS="$(sdkconfig_defaults_arg)" "$@"
}

ensure_target() {
    ensure_idf_env
    if [[ -f "${PROJECT_DIR}/sdkconfig" ]] && grep -q "^CONFIG_IDF_TARGET=\"${DEFAULT_TARGET}\"$" "${PROJECT_DIR}/sdkconfig"; then
        return 0
    fi
    run_idf_profiled set-target "${DEFAULT_TARGET}" >/dev/null
}

run_idf() {
    ensure_target
    idf.py -C "${PROJECT_DIR}" "$@"
}

command_name="${1:-build}"
load_config_env

case "${command_name}" in
    configure)
        configure_project 1
        ;;
    port)
        detect_port
        ;;
    build)
        ensure_project_config
        run_idf build
        ;;
    clean)
        run_idf clean
        ;;
    fullclean)
        run_idf fullclean
        ;;
    menuconfig)
        ensure_project_config
        run_idf menuconfig
        ;;
    flash)
        ensure_project_config
        port="$(detect_port)"
        echo "Using serial port: ${port}" >&2
        run_idf -p "${port}" flash
        ;;
    monitor)
        port="$(detect_port)"
        echo "Using serial port: ${port}" >&2
        run_idf -p "${port}" monitor
        ;;
    flash-monitor)
        ensure_project_config
        port="$(detect_port)"
        echo "Using serial port: ${port}" >&2
        run_idf -p "${port}" flash monitor
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
