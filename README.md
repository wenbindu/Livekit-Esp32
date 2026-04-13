# livekit-esp32s3

English | [简体中文](README.zh-CN.md)

`livekit-esp32s3` is a standalone ESP-IDF project for the `lichuang_esp32s3` board. It keeps the Lichuang-specific audio, Wi-Fi provisioning, and LCD pipeline, while restructuring the original prototype into an open-source-friendly repository.

## What This Project Does

- connects an ESP32-S3 device to a LiveKit room
- captures microphone audio and publishes it as Opus
- subscribes to remote audio and renders it on the onboard speaker
- provides a lightweight Wi-Fi provisioning portal
- keeps Lichuang board support, display UI, and voice-processing path in one repo

## Repository Layout

- `main/`: firmware app, board glue, LiveKit integration, UI, Wi-Fi flow
- `components/78__esp-wifi-connect`: vendored provisioning portal component
- `components/78__xiaozhi-fonts`: vendored font and emoji assets
- `configs/`: board/profile defaults plus the local config example
- `configs/scenarios/`: scenario overlays such as `dev-chat`, `debug-jwt`, `dev-audio-ws`, and `release-token`
- `docs/`: development and packaging notes
- `scripts/project.sh`: configure/build/flash wrapper
- `scripts/package_firmware.sh`: package scenario-specific firmware artifacts
- `scripts/token_server.py`: local or remote token service
- `scripts/debug_uplink_ws_server.py`: desktop receiver for debug-audio WAV capture

## Open-Source Safety

Real credentials must stay in:

- `configs/livekit.local.env`

That file is gitignored. The tracked file:

- `configs/livekit.local.env.example`

contains placeholders only.

Important:

- `AUTH_MODE=token_server` is the recommended mode for development and production.
- `AUTH_MODE=device_jwt` is development-only. It embeds `LIVEKIT_API_SECRET` into the generated firmware image.
- Do not commit real tokens, API keys, or machine-local IPs into scenario files, Markdown docs, or tracked defaults.

## Tested Environment

This project is currently organized around:

- ESP-IDF `v5.5.3`
- target `esp32s3`
- board `lichuang_esp32s3`
- Python from the ESP-IDF environment

## Development Environment Setup

### 1. Install ESP-IDF

Example flow:

```bash
git clone https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.5.3
bash install.sh
. ./export.sh
idf.py --version
```

Optional sanity check:

```bash
cd examples/get-started/hello_world
idf.py build
```

### 2. Clone This Project

```bash
git clone <your-repo-url> livekit-esp32s3
cd livekit-esp32s3
```

### 3. Create The Local Config

```bash
cp configs/livekit.local.env.example configs/livekit.local.env
```

Then edit `configs/livekit.local.env`.

Minimum fields depend on auth mode:

- `token_server`: `TOKEN_SERVER_URL`, plus the token server itself needs `LIVEKIT_URL`, `LIVEKIT_API_KEY`, `LIVEKIT_API_SECRET`
- `device_jwt`: `LIVEKIT_URL`, `LIVEKIT_API_KEY`, `LIVEKIT_API_SECRET`
- `sandbox`: `LIVEKIT_SANDBOX_ID`
- `static_token`: `LIVEKIT_URL`, `LIVEKIT_TOKEN`

Useful token-server knobs:

- `TOKEN_SERVER_RETRY_DELAY_MS`
- `TOKEN_SERVER_AUTH_MAX_FAILURES`

### 4. Build And Flash

Normal development chat firmware:

```bash
SCENARIO=dev-chat bash scripts/project.sh flash-monitor
```

Lower-overhead device-JWT firmware:

```bash
SCENARIO=debug-jwt bash scripts/project.sh flash-monitor
```

Dual-path debug-audio firmware:

```bash
SCENARIO=dev-audio-ws bash scripts/project.sh flash-monitor
```

Release token-server firmware:

```bash
SCENARIO=release-token bash scripts/project.sh flash-monitor
```

## Scenario Model

This repo does not use separate git branches for `dev` and `prod`.

It uses four layers:

1. board defaults
2. profile defaults
3. scenario overlay
4. local env

Useful scenarios:

- `dev-chat`: default daily development firmware
- `debug-jwt`: no debug audio WS export, device-generated JWT, lighter dev path
- `dev-uplink-ws`: export processed uplink audio to a desktop receiver
- `dev-audio-ws`: export both uplink and downlink audio for WAV inspection
- `dev-uplink-only`: local uplink diagnostics without normal room chat
- `prod-standby`: production-like standby-first behavior
- `release-token`: release firmware with server-side JWT signing and device-side token refresh

See:

- `docs/profiles.md`
- `docs/firmware-packaging.md`
- `docs/debug-audio.md`
- `docs/debug-jwt.md`
- `docs/release-token.md`

## Lichuang Board Notes

The repo is currently tuned for the Lichuang ESP32-S3 development board.

- default board is `lichuang_esp32s3`
- serial port is usually exposed over USB Type-C as `/dev/cu.usbmodem*` on macOS
- target is `esp32s3`
- the BOOT button is used for Wi-Fi provisioning entry and chat interaction
- the current playback path is intentionally kept as app-level mono PCM for the single-speaker hardware
- Wi-Fi provisioning is part of the firmware flow; if saved Wi-Fi fails, the board should return to provisioning mode

## Token Server

Recommended for normal development and production:

```bash
cp configs/token_server.local.env.example configs/token_server.local.env
python3 scripts/token_server.py --env-file configs/token_server.local.env
```

Run it in the background with PID/log management:

```bash
bash scripts/token_server_ctl.sh start
```

Use separate files for device and server:

- `configs/livekit.local.env`: device build config, token server URL, debug endpoints
- `configs/token_server.local.env`: token server runtime secrets, LiveKit API key/secret

Stop or inspect it:

```bash
bash scripts/token_server_ctl.sh status
bash scripts/token_server_ctl.sh stop
```

For the release firmware flow that refreshes token-server JWTs and shows `AUTH EXPIRED` after repeated auth failures, see:

- `docs/release-token.md`

If the board cannot reach your token server, use `SCENARIO=debug-jwt` temporarily.

## Debug-Audio Workflow

Start the desktop receiver:

```bash
bash scripts/debug_audio_ws_start.sh
```

Stop it:

```bash
bash scripts/debug_audio_ws_stop.sh
```

Generated WAV files are written under `debug_audio_ws/`, which is gitignored.

## Packaging

List supported scenarios:

```bash
bash scripts/package_firmware.sh list
```

Package a scenario:

```bash
bash scripts/package_firmware.sh dev-chat
```

Examples:

```bash
bash scripts/package_firmware.sh debug-jwt
bash scripts/package_firmware.sh dev-audio-ws
bash scripts/package_firmware.sh release-token
```

## Practical Notes

- LiveKit Cloud reachability still depends on the network used by the board.
- If the web client also cannot talk to the agent, stop changing firmware first and inspect the agent logs.
- For production deployment, prefer token-server auth and keep the API secret off the device.
