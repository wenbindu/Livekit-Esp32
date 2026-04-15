#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_TARGET="current"
DEFAULT_DIST_DIR="${PROJECT_DIR}/dist"
DEFAULT_ENV_FILE="${PROJECT_DIR}/configs/livekit.local.env"
DEFAULT_BRANCH_ENV_FILE="${PROJECT_DIR}/configs/branch.defaults.env"
DEFAULT_PRESET_DIR="${PROJECT_DIR}/configs/presets"

usage() {
    cat <<'EOF'
Usage:
  scripts/package_firmware.sh
  scripts/package_firmware.sh list
  scripts/package_firmware.sh presets
  scripts/package_firmware.sh preset <name>

Examples:
  bash scripts/package_firmware.sh
  bash scripts/package_firmware.sh list
  bash scripts/package_firmware.sh preset audio-trace

Environment:
  CONFIG_ENV_FILE     Local env file, default: configs/livekit.local.env
  DIST_DIR            Output directory, default: dist/
  FIRMWARE_PRESET     Optional diagnostic preset from configs/presets/<name>.env

Current workflow:
  package_firmware.sh packages the current lifecycle branch.
  Optional diagnostic behavior comes from a preset overlay.
EOF
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

load_runtime_env() {
    local env_file="${CONFIG_ENV_FILE:-${DEFAULT_ENV_FILE}}"
    load_optional_env "${env_file}"
    load_optional_env "${DEFAULT_BRANCH_ENV_FILE}"

    local preset_file=""
    preset_file="$(resolve_preset_env_file)"
    if [[ -n "${preset_file}" ]]; then
        load_optional_env "${preset_file}"
    fi
}

list_identity() {
    local branch_name
    branch_name="$(git -C "${PROJECT_DIR}" rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'detached')"
    local preset_file=""
    preset_file="$(resolve_preset_env_file)"
    load_runtime_env
    printf 'branch=%s\n' "${branch_name}"
    printf 'firmware_variant=%s\n' "${FIRMWARE_VARIANT:-${branch_name}}"
    printf 'profile=%s\n' "${PROFILE:-dev}"
    printf 'preset=%s\n' "${FIRMWARE_PRESET:-}"
    printf 'preset_env_file=%s\n' "${preset_file}"
}

list_presets() {
    if [[ ! -d "${DEFAULT_PRESET_DIR}" ]]; then
        return 0
    fi

    find "${DEFAULT_PRESET_DIR}" -maxdepth 1 -type f -name '*.env' | sort | while IFS= read -r file; do
        basename "${file}" .env
    done
}

copy_if_exists() {
    local src="$1"
    local dst_dir="$2"
    if [[ -f "${src}" ]]; then
        cp "${src}" "${dst_dir}/"
    fi
}

main() {
    local command="${1:-${DEFAULT_TARGET}}"
    if [[ "${command}" == "list" ]]; then
        list_identity
        return 0
    fi
    if [[ "${command}" == "presets" ]]; then
        list_presets
        return 0
    fi
    if [[ "${command}" == "-h" || "${command}" == "--help" || "${command}" == "help" ]]; then
        usage
        return 0
    fi
    if [[ "${command}" == "preset" ]]; then
        if [[ -z "${2:-}" ]]; then
            echo "Usage: scripts/package_firmware.sh preset <name>" >&2
            return 1
        fi
        export FIRMWARE_PRESET="${2}"
        command="current"
    fi
    if [[ "${command}" != "current" ]]; then
        echo "Unknown packaging target: ${command}" >&2
        echo "package_firmware.sh only packages the current lifecycle branch." >&2
        return 1
    fi

    local env_file="${CONFIG_ENV_FILE:-${DEFAULT_ENV_FILE}}"

    if [[ ! -f "${env_file}" ]]; then
        echo "Missing local env: ${env_file}" >&2
        echo "Copy configs/livekit.local.env.example first." >&2
        return 1
    fi

    local preset_file=""
    preset_file="$(resolve_preset_env_file)"
    load_runtime_env

    local branch_name
    branch_name="$(git -C "${PROJECT_DIR}" rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'detached')"
    local variant_name="${FIRMWARE_VARIANT:-${branch_name}}"
    export CONFIG_ENV_FILE="${env_file}"

    bash "${SCRIPT_DIR}/project.sh" configure
    bash "${SCRIPT_DIR}/project.sh" build

    local timestamp
    timestamp="$(date +%Y%m%d_%H%M%S)"
    local board="${BOARD:-lichuang_esp32s3}"
    local profile="${PROFILE:-dev}"
    local dist_dir="${DIST_DIR:-${DEFAULT_DIST_DIR}}"
    local artifact_dir="${dist_dir}/${timestamp}_${board}_${profile}_${variant_name}"
    if [[ -n "${FIRMWARE_PRESET:-}" ]]; then
        artifact_dir="${artifact_dir}_preset-${FIRMWARE_PRESET}"
    fi
    mkdir -p "${artifact_dir}"

    copy_if_exists "${PROJECT_DIR}/build/bootloader/bootloader.bin" "${artifact_dir}"
    copy_if_exists "${PROJECT_DIR}/build/partition_table/partition-table.bin" "${artifact_dir}"
    copy_if_exists "${PROJECT_DIR}/build/livekit_esp32s3.bin" "${artifact_dir}"
    copy_if_exists "${PROJECT_DIR}/build/flash_args" "${artifact_dir}"
    copy_if_exists "${PROJECT_DIR}/build/flasher_args.json" "${artifact_dir}"
    copy_if_exists "${PROJECT_DIR}/sdkconfig" "${artifact_dir}"
    copy_if_exists "${PROJECT_DIR}/sdkconfig.defaults.generated" "${artifact_dir}"

    cat > "${artifact_dir}/manifest.txt" <<EOF
branch=${branch_name}
firmware_variant=${variant_name}
board=${board}
profile=${profile}
preset=${FIRMWARE_PRESET:-}
auth_mode=${AUTH_MODE:-}
token_server_url=${TOKEN_SERVER_URL:-}
livekit_url=${LIVEKIT_URL:-}
room=${LIVEKIT_ROOM:-}
participant=${LIVEKIT_PARTICIPANT:-}
agent_name=${LIVEKIT_AGENT_NAME:-}
debug_uplink_ws=${ENABLE_DEBUG_UPLINK_WS:-0}
debug_downlink_ws=${ENABLE_DEBUG_DOWNLINK_WS:-0}
debug_downlink_http_upload=${ENABLE_DEBUG_DOWNLINK_HTTP_UPLOAD:-0}
debug_uplink_wav=${ENABLE_DEBUG_UPLINK_WAV:-0}
local_audio_uplink_only=${LOCAL_AUDIO_UPLINK_ONLY:-0}
start_in_standby=${START_IN_STANDBY:-0}
branch_defaults_file=${DEFAULT_BRANCH_ENV_FILE}
preset_env_file=${preset_file}
config_env_file=${env_file}
generated_at=${timestamp}
EOF

    echo "Packaged firmware -> ${artifact_dir}"
}

main "$@"
