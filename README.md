# livekit-esp32s3

`livekit-esp32s3` is a self-contained ESP-IDF project for the `lichuang_esp32s3` board. It keeps the Lichuang-specific audio, Wi-Fi provisioning, and display pipeline, while cleaning up the structure so the project can be published as a standalone open-source repository.

## What Is Included

- `main/`: board app, media pipeline, UI, Wi-Fi provisioning, LiveKit integration
- `components/78__esp-wifi-connect`: local copy of the provisioning portal component
- `components/78__xiaozhi-fonts`: local copy of the display font/emoji assets
- `configs/`: board defaults, dev/prod profile overlays, local config example
- `configs/scenarios/`: use-case overlays for packaged firmware variants
- `scripts/project.sh`: configure/build/flash wrapper
- `scripts/package_firmware.sh`: scenario-based firmware packaging helper
- `scripts/token_server.py`: local or remote token service
- `scripts/debug_uplink_ws_server.py`: dev-only PCM receiver for processed uplink and rendered downlink
- `scripts/debug_audio_ws_start.sh`: start the local debug-audio receiver in the background
- `scripts/debug_audio_ws_stop.sh`: stop the local debug-audio receiver

## Why The Layout Changed

The previous evaluation project depended on sibling directories and parent `managed_components`. That works in a monorepo, but it is not suitable for an open-source repo. This version vendors the Lichuang-specific components it needs, keeps the firmware app self-contained, and moves secrets into an ignored local config file.

## Configuration Model

This project does not use git branches to switch between development and production.

It uses four layers instead:

1. `board` overlay: hardware-specific defaults such as flash/PSRAM target
2. `profile` overlay: `dev` vs `prod`
3. `scenario` overlay: use-case behavior such as `dev-chat` or `prod-standby`
4. `local env`: secrets and machine-local endpoints in `configs/livekit.local.env`

This is the mature pattern for embedded products because it avoids branch drift and makes builds reproducible.

## Auth Modes

Supported firmware auth modes:

- `device_jwt`: dev-only fallback when the board cannot reach your token server
- `token_server`: recommended for dev and prod
- `sandbox`: convenient for LiveKit Cloud sandbox experiments
- `static_token`: dev-only fallback

`device_jwt` requires a valid device clock because the JWT includes `nbf` and `exp`. This project syncs time over SNTP before joining the room.

The recommended model is still to keep `LIVEKIT_API_SECRET` off the device. Run `scripts/token_server.py` locally during development or deploy the same contract remotely in production.

## Quick Start

1. Copy the local config example:

```bash
cp configs/livekit.local.env.example configs/livekit.local.env
```

2. Edit `configs/livekit.local.env`.

If your board cannot reach a local token server, set `AUTH_MODE=device_jwt` and provide:

- `LIVEKIT_URL`
- `LIVEKIT_API_KEY`
- `LIVEKIT_API_SECRET`

3. Configure and build:

```bash
SCENARIO=dev-chat bash scripts/project.sh configure
SCENARIO=dev-chat bash scripts/project.sh build
```

4. Flash:

```bash
SCENARIO=dev-chat bash scripts/project.sh flash-monitor
```

## Scenario Packaging

List supported scenarios:

```bash
bash scripts/package_firmware.sh list
```

Package a firmware drop:

```bash
bash scripts/package_firmware.sh dev-chat
```

See `docs/firmware-packaging.md` for the scenario matrix and artifact layout.

## Dev Tooling

Start the local token server:

```bash
python3 scripts/token_server.py --env-file configs/livekit.local.env
```

Start the dev-only debug-audio receiver:

```bash
bash scripts/debug_audio_ws_start.sh
```

Stop it:

```bash
bash scripts/debug_audio_ws_stop.sh
```

Build and flash the dedicated debug-audio firmware:

```bash
SCENARIO=dev-audio-ws bash scripts/project.sh flash-monitor
```

Package the same firmware as a distributable artifact:

```bash
bash scripts/package_firmware.sh dev-audio-ws
```

## Power And UX Direction

For production, the recommended baseline is:

- boot
- connect Wi-Fi
- sync time
- stay in standby
- join room only after BOOT button press
- return to standby after the chat session ends

This removes unnecessary room connections and keeps the device lighter than always-online dev builds. See `docs/profiles.md`.
