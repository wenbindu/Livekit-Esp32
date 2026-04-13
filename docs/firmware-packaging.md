# Firmware Packaging

## Packaging Rule

Package the firmware represented by the current git branch.

Firmware behavior now comes from:

1. `configs/sdkconfig.defaults.board.<board>`
2. `configs/sdkconfig.defaults.<profile>`
3. `configs/branch.defaults.env`
4. `configs/livekit.local.env`

## Branch Map

- `main`: normal daily firmware
- `fw-dev-uplink-ws`: processed uplink debug export
- `fw-dev-audio-ws`: uplink + downlink debug export
- `fw-prod-standby`: production-like standby behavior
- `fw-release-token`: release-oriented standby path

## Commands

Build and flash the current branch:

```bash
bash scripts/project.sh flash-monitor
```

Package the current branch:

```bash
bash scripts/package_firmware.sh
```

Inspect the current branch package identity:

```bash
bash scripts/package_firmware.sh list
```

Example debug-audio package:

```bash
git switch fw-dev-audio-ws
bash scripts/package_firmware.sh
```

Example release package:

```bash
git switch fw-release-token
bash scripts/package_firmware.sh
```

Each package is written to:

- `dist/<timestamp>_<board>_<profile>_<firmware_variant>/`

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
- `configs/token_server.local.env`

Do not place real credentials in:

- `configs/livekit.local.env.example`
- `configs/token_server.local.env.example`
- `configs/branch.defaults.env`
- tracked Markdown docs
