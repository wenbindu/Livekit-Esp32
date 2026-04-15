# Firmware Packaging

## Packaging Rule

Package the firmware represented by the current lifecycle branch. Add a preset
only when you intentionally need a diagnostic artifact.

Firmware behavior comes from:

1. `configs/sdkconfig.defaults.board.<board>`
2. `configs/sdkconfig.defaults.<profile>`
3. `configs/branch.defaults.env`
4. `configs/presets/<preset>.env` when `FIRMWARE_PRESET` is set
5. `configs/livekit.local.env`

## Branch Map

- `dev`: daily development firmware
- `test`: integration and pre-release firmware
- `main`: production baseline firmware

## Preset Map

- `uplink-trace`: processed uplink audio export over WebSocket
- `audio-trace`: uplink + downlink audio export over WebSocket
- `downlink-http`: rendered downlink audio uploaded to device_server as WAV blobs

## Commands

Build and flash the current branch:

```bash
bash scripts/project.sh flash-monitor
```

Build and flash with a diagnostic preset:

```bash
FIRMWARE_PRESET=audio-trace bash scripts/project.sh flash-monitor
```

Package the current branch:

```bash
bash scripts/package_firmware.sh
```

Package the current branch with a diagnostic preset:

```bash
bash scripts/package_firmware.sh preset audio-trace
```

Inspect the current package identity:

```bash
bash scripts/package_firmware.sh list
```

List available presets:

```bash
bash scripts/package_firmware.sh presets
```

Each package is written to:

- `dist/<timestamp>_<board>_<profile>_<firmware_variant>/`
- `dist/<timestamp>_<board>_<profile>_<firmware_variant>_preset-<name>/` when a preset is used

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
- `device_server/configs/device_server.local.env`

Do not place real credentials in:

- `configs/livekit.local.env.example`
- `device_server/configs/device_server.local.env.example`
- `configs/branch.defaults.env`
- `configs/presets/*.env`
- tracked Markdown docs
