# Profiles And Modes

## Recommended Build Strategy

Do not use `git branch` as the environment switch for `dev` vs `prod`.

Use one branch and three config layers:

- `configs/sdkconfig.defaults.board.<board>` for hardware
- `configs/sdkconfig.defaults.<profile>` for behavior
- `configs/livekit.local.env` for secrets and local endpoints

For use-case-specific firmware variants, add a fourth layer:

- `configs/scenarios/*.env`

This avoids branch drift, makes CI simpler, and keeps firmware diffs focused on code instead of environment churn.

## Development Profile

Goals:

- fast iteration
- visibility into audio quality
- easier auth changes

Recommended toggles:

- `AUTH_MODE=token_server`
- `START_IN_STANDBY=0`

Recommended scenarios:

- `dev-token`
- `dev-uplink-ws`
- `dev-audio-ws`

All development scenarios in this repo use `AUTH_MODE=token_server`.
For the dual-path WAV capture workflow, see `docs/debug-audio.md`.

## Production Profile

Goals:

- lower power
- fewer background tasks
- no debug audio export
- better user flow

Recommended toggles:

- `AUTH_MODE=token_server`
- `START_IN_STANDBY=1`

Recommended scenario:

- `prod-standby`
- `release-token`

## Standby Flow

Current practical recommendation:

1. Boot device
2. Connect Wi-Fi
3. Stay in standby screen
4. Wait for BOOT button press
5. Sync time and initialize audio
6. Join room and start chat
7. Leave room and return to standby

This is lighter and less error-prone than immediately connecting to a room after every reboot.

## Deep Sleep Recommendation

If you later add a dedicated wake/power button and stable resume flow, move from standby to true deep sleep. Until then, standby is the safer open-source default because it preserves a simpler recovery path and avoids more board-specific power management code.

For packaging commands and the scenario matrix, see `docs/firmware-packaging.md`.
