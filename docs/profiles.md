# Profiles And Modes

## Recommended Build Strategy

Use `git branch` for firmware-path switching, and keep profile semantics inside
the branch-owned defaults.

Common layers:

- `configs/sdkconfig.defaults.board.<board>` for hardware
- `configs/sdkconfig.defaults.<profile>` for behavior
- `configs/branch.defaults.env` for branch-owned firmware defaults
- `configs/livekit.local.env` for secrets and local endpoints

Mainline firmware stays on `main`. Dedicated firmware variants live on their own
branches.

## Development Profile

Goals:

- fast iteration
- visibility into audio quality
- easier auth changes

Recommended toggles:

- `AUTH_MODE=token_server`
- `START_IN_STANDBY=0`

Recommended branches:

- `main`
- `firmware/dev-uplink-ws`
- `firmware/dev-audio-ws`

All development branches in this repo use `AUTH_MODE=token_server`.
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

Recommended branches:

- `firmware/prod-standby`
- `firmware/release-token`

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

For packaging commands and the branch map, see `docs/firmware-packaging.md`.
