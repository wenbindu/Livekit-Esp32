#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_TARGET="current"
DEFAULT_DIST_DIR="${PROJECT_DIR}/dist"
DEFAULT_ENV_FILE="${PROJECT_DIR}/configs/livekit.local.env"
DEFAULT_BRANCH_ENV_FILE="${PROJECT_DIR}/configs/branch.defaults.env"
DEFAULT_SCENARIO_DIR="${PROJECT_DIR}/configs/scenarios"

usage() {
    cat <<'EOF'
Usage:
  scripts/package_firmware.sh
  scripts/package_firmware.sh list
  scripts/package_firmware.sh [legacy-scenario]

Examples:
  bash scripts/package_firmware.sh
  bash scripts/package_firmware.sh list

Environment:
  CONFIG_ENV_FILE     Local env file, default: configs/livekit.local.env
  SCENARIO_ENV_FILE   Legacy explicit scenario env file override
  DIST_DIR            Output directory, default: dist/

Current workflow:
  package_firmware.sh packages the firmware represented by the current git branch.
  The branch-owned defaults live in configs/branch.defaults.env.
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

list_scenarios() {
    local branch_name
    branch_name="$(git -C "${PROJECT_DIR}" rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'detached')"
    load_optional_env "${CONFIG_ENV_FILE:-${DEFAULT_ENV_FILE}}"
    load_optional_env "${DEFAULT_BRANCH_ENV_FILE}"
    printf 'branch=%s\n' "${branch_name}"
    printf 'firmware_variant=%s\n' "${FIRMWARE_VARIANT:-${branch_name}}"
    printf 'profile=%s\n' "${PROFILE:-dev}"

    local file
    for file in "${DEFAULT_SCENARIO_DIR}"/*.env; do
        [[ -e "${file}" ]] || continue
        printf 'legacy:%s\n' "$(basename "${file}" .env)"
    done | sort
}

resolve_scenario_file() {
    local scenario="$1"
    if [[ -n "${SCENARIO_ENV_FILE:-}" ]]; then
        printf '%s\n' "${SCENARIO_ENV_FILE}"
        return 0
    fi
    printf '%s/%s.env\n' "${DEFAULT_SCENARIO_DIR}" "${scenario}"
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
        list_scenarios
        return 0
    fi
    if [[ "${command}" == "-h" || "${command}" == "--help" || "${command}" == "help" ]]; then
        usage
        return 0
    fi

    local scenario="${command}"
    local env_file="${CONFIG_ENV_FILE:-${DEFAULT_ENV_FILE}}"
    local scenario_file=""
    local legacy_scenario=0

    if [[ ! -f "${env_file}" ]]; then
        echo "Missing local env: ${env_file}" >&2
        echo "Copy configs/livekit.local.env.example first." >&2
        return 1
    fi
    if [[ "${scenario}" != "current" ]]; then
        scenario_file="$(resolve_scenario_file "${scenario}")"
        if [[ ! -f "${scenario_file}" ]]; then
            echo "Unknown packaging target: ${scenario}" >&2
            echo "Use the current git branch, or pick one of the legacy scenario files:" >&2
            list_scenarios >&2
            return 1
        fi
        legacy_scenario=1
    fi

    load_optional_env "${env_file}"
    load_optional_env "${DEFAULT_BRANCH_ENV_FILE}"
    if [[ "${legacy_scenario}" == "1" ]]; then
        load_optional_env "${scenario_file}"
    fi

    local branch_name
    branch_name="$(git -C "${PROJECT_DIR}" rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'detached')"
    local variant_name="${FIRMWARE_VARIANT:-${branch_name}}"
    if [[ "${legacy_scenario}" == "1" ]]; then
        variant_name="${scenario}"
        export SCENARIO="${scenario}"
        export SCENARIO_ENV_FILE="${scenario_file}"
    else
        unset SCENARIO || true
        unset SCENARIO_ENV_FILE || true
    fi
    export CONFIG_ENV_FILE="${env_file}"

    bash "${SCRIPT_DIR}/project.sh" configure
    bash "${SCRIPT_DIR}/project.sh" build

    local timestamp
    timestamp="$(date +%Y%m%d_%H%M%S)"
    local board="${BOARD:-lichuang_esp32s3}"
    local profile="${PROFILE:-dev}"
    local dist_dir="${DIST_DIR:-${DEFAULT_DIST_DIR}}"
    local artifact_dir="${dist_dir}/${timestamp}_${board}_${profile}_${variant_name}"
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
auth_mode=${AUTH_MODE:-}
token_server_url=${TOKEN_SERVER_URL:-}
livekit_url=${LIVEKIT_URL:-}
room=${LIVEKIT_ROOM:-}
participant=${LIVEKIT_PARTICIPANT:-}
agent_name=${LIVEKIT_AGENT_NAME:-}
debug_uplink_ws=${ENABLE_DEBUG_UPLINK_WS:-0}
debug_downlink_ws=${ENABLE_DEBUG_DOWNLINK_WS:-0}
debug_uplink_wav=${ENABLE_DEBUG_UPLINK_WAV:-0}
local_audio_uplink_only=${LOCAL_AUDIO_UPLINK_ONLY:-0}
start_in_standby=${START_IN_STANDBY:-0}
branch_defaults_file=${DEFAULT_BRANCH_ENV_FILE}
scenario_env_file=${scenario_file}
config_env_file=${env_file}
generated_at=${timestamp}
EOF

    echo "Packaged firmware -> ${artifact_dir}"
}

main "$@"
