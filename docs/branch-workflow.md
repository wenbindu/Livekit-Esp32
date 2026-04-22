# Branch Workflow

## Goal

Different firmware lifecycle states are managed by branch, while temporary
diagnostic behavior is managed by preset.

Lifecycle branches are:

- `dev`
- `test`
- `main`

Diagnostic presets are loaded only when needed:

- `uplink-trace`
- `audio-trace`
- `downlink-http`

## How It Works

- `configs/livekit.local.env`: machine-local secrets and endpoints, never committed
- `configs/branch.defaults.env`: tracked lifecycle defaults owned by the current branch
- `configs/presets/<name>.env`: optional diagnostic overlay selected with `FIRMWARE_PRESET`

`scripts/project.sh` and `scripts/package_firmware.sh` automatically load the
local env and the current branch defaults. When `FIRMWARE_PRESET` is set, they
also load the preset overlay after the branch defaults.

## Daily Commands

Build and flash the current lifecycle branch:

```bash
bash scripts/project.sh flash-monitor
```

Build and flash with a temporary diagnostic preset:

```bash
FIRMWARE_PRESET=audio-trace bash scripts/project.sh flash-monitor
```

For downlink crackle capture without a workstation WebSocket receiver:

```bash
FIRMWARE_PRESET=downlink-http bash scripts/project.sh flash-monitor
```

Package the current lifecycle branch:

```bash
bash scripts/package_firmware.sh
```

Package the current lifecycle branch with a diagnostic preset:

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

## Promotion Rule

Functional work should move through:

1. `dev`
2. `test`
3. `main`

Do not keep functional fixes inside a temporary debug-only branch or preset-only
artifact.
