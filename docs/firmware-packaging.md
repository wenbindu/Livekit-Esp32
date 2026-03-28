# Firmware Packaging

## Single-Branch Rule

Do not create separate git branches for `dev` and `prod` firmware.

Use one branch and four layers:

1. `board`
2. `profile`
3. `scenario`
4. `local env`

This keeps code review focused on real firmware changes while still producing different firmware builds for different use cases.

## Layer Responsibilities

- `configs/sdkconfig.defaults.board.<board>`: hardware-specific settings
- `configs/sdkconfig.defaults.<profile>`: broad runtime posture such as `dev` vs `prod`
- `configs/scenarios/*.env`: use-case behavior such as debug audio capture or standby
- `configs/livekit.local.env`: machine-local secrets and endpoints

## Recommended Scenarios

### `dev-chat`

Use when the goal is normal daily firmware debugging.

- `PROFILE=dev`
- no debug uplink export
- immediate join flow

### `dev-uplink-ws`

Use when checking processed uplink audio on a desktop receiver.

- `PROFILE=dev`
- `ENABLE_DEBUG_UPLINK_WS=1`
- full chat still enabled

### `dev-audio-ws`

Use when checking both processed microphone uplink and rendered AI downlink on a desktop receiver.

- `PROFILE=dev`
- `ENABLE_DEBUG_UPLINK_WS=1`
- `ENABLE_DEBUG_DOWNLINK_WS=1`
- full chat still enabled
- this is the recommended `debug-audio` firmware variant

Detailed workflow:

- `docs/debug-audio.md`

### `debug-jwt`

Use when you want a lighter development firmware:

- `PROFILE=dev`
- `AUTH_MODE=device_jwt`
- no debug uplink export
- no debug downlink export
- immediate join flow
- lower local debug overhead than `dev-audio-ws`

Detailed workflow:

- `docs/debug-jwt.md`

### `dev-uplink-only`

Use when isolating microphone, AEC, AGC, and uplink processing.

- `PROFILE=dev`
- `ENABLE_DEBUG_UPLINK_WS=1`
- `LOCAL_AUDIO_UPLINK_ONLY=1`

### `prod-standby`

Use for production-like packaging.

- `PROFILE=prod`
- no debug uplink export
- enter standby first
- join only after user action

### `release-token`

Use for the actual release firmware that keeps LiveKit secrets off the device.

- `PROFILE=prod`
- `AUTH_MODE=token_server`
- no debug audio export
- enter standby first
- fetch a fresh token from the token server before room join
- when the token is invalid or expired, fetch a new token and retry
- after repeated auth failures, show `AUTH EXPIRED` on the device

## Everyday Commands

Build and flash with a scenario:

```bash
SCENARIO=dev-chat bash scripts/project.sh flash-monitor
```

Switch to lower-overhead device JWT debugging:

```bash
SCENARIO=debug-jwt bash scripts/project.sh flash-monitor
```

Switch to processed uplink debugging:

```bash
SCENARIO=dev-uplink-ws bash scripts/project.sh flash-monitor
```

Switch to dual-path audio debugging:

```bash
SCENARIO=dev-audio-ws bash scripts/project.sh flash-monitor
```

Start the desktop WAV receiver:

```bash
bash scripts/debug_audio_ws_start.sh
```

Stop it:

```bash
bash scripts/debug_audio_ws_stop.sh
```

Build a production-like standby image:

```bash
SCENARIO=prod-standby bash scripts/project.sh build
```

Build the release token-server image:

```bash
SCENARIO=release-token bash scripts/project.sh build
```

## Packaging Commands

List built-in scenarios:

```bash
bash scripts/package_firmware.sh list
```

Produce a distributable firmware folder:

```bash
bash scripts/package_firmware.sh dev-chat
```

Example lighter device-JWT debug package:

```bash
bash scripts/package_firmware.sh debug-jwt
```

Example debug-audio package:

```bash
bash scripts/package_firmware.sh dev-audio-ws
```

Example release package:

```bash
bash scripts/package_firmware.sh release-token
```

This is the recommended way to freeze a dedicated audio-debug firmware without touching the normal chat firmware path.

Each package is written to `dist/<timestamp>_<board>_<profile>_<scenario>/`.

Included files:

- `bootloader.bin`
- `partition-table.bin`
- `livekit_esp32s3.bin`
- `flash_args`
- `flasher_args.json` when available
- `sdkconfig`
- `sdkconfig.defaults.generated`
- `manifest.txt`

## Local Secrets Rule

Keep all real LiveKit secrets only in:

- `configs/livekit.local.env`

Do not place real credentials in:

- `configs/livekit.local.env.example`
- scenario files
- tracked Markdown docs

## Current Development Setup

The ignored local env can safely keep:

- `LIVEKIT_URL`
- `LIVEKIT_API_KEY`
- `LIVEKIT_API_SECRET`
- `TOKEN_SERVER_URL`
- `DEBUG_UPLINK_WS_URL`
- `DEBUG_DOWNLINK_WS_URL`

`debug-jwt` expects these local secrets to be present:

- `LIVEKIT_URL`
- `LIVEKIT_API_KEY`
- `LIVEKIT_API_SECRET`

Use `AUTH_MODE=device_jwt` only when the board cannot reach your token server and you accept embedding the secret into the dev firmware.

For normal dev and production packaging, prefer `AUTH_MODE=token_server`.
