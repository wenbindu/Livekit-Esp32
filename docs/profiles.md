# Profiles And Modes

## Recommended Build Strategy

Use branch for lifecycle and preset for diagnostics.

Common layers:

- `configs/sdkconfig.defaults.board.<board>` for hardware
- `configs/sdkconfig.defaults.<profile>` for behavior
- `configs/branch.defaults.env` for lifecycle defaults
- `configs/presets/<preset>.env` for temporary diagnostic overlays
- `configs/livekit.local.env` for secrets and local endpoints

## Development Branch

Branch:

- `dev`

Default behavior:

- `PROFILE=dev`
- `AUTH_MODE=token_server`
- `START_IN_STANDBY=0`
- debug audio disabled by default

Use this branch for day-to-day development.

Recommended presets when investigating audio issues:

- `uplink-trace`
- `audio-trace`
- `downlink-http`

## Test Branch

Branch:

- `test`

Default behavior:

- `PROFILE=test`
- `AUTH_MODE=token_server`
- `START_IN_STANDBY=1`
- debug audio disabled by default

Use this branch for integration, regression checks, and pre-release validation.

## Production Branch

Branch:

- `main`

Default behavior:

- `PROFILE=prod`
- `AUTH_MODE=token_server`
- `START_IN_STANDBY=1`
- no debug audio export

Use this branch as the production baseline.

## Standby Flow

Current practical recommendation:

1. Boot device
2. Connect Wi-Fi
3. Stay in standby screen
4. Wait for BOOT button press
5. Sync time and initialize audio
6. Join room and start chat
7. Leave room and return to standby

This is lighter and less error-prone than immediately connecting to a room after
every reboot.

## Deep Sleep Recommendation

If you later add a dedicated wake/power button and stable resume flow, move from
standby to true deep sleep. Until then, standby is the safer open-source
default because it preserves a simpler recovery path and avoids more
board-specific power management code.

## Related Docs

- `docs/branch-workflow.md`
- `docs/firmware-packaging.md`
- `docs/debug-audio.md`
- `docs/firmware-lifecycle-design.md`
