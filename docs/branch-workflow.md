# Branch Workflow

## Goal

Different firmware paths are now managed by git branch, not by passing different
`SCENARIO=...` values on the same branch.

Mainline firmware stays on:

- `main`

Dedicated firmware branches are:

- `firmware/dev-uplink-ws`
- `firmware/dev-audio-ws`
- `firmware/prod-standby`
- `firmware/release-token`

## How It Works

- `configs/livekit.local.env`: machine-local secrets and endpoints, never committed
- `configs/branch.defaults.env`: tracked defaults owned by the current branch

`scripts/project.sh` automatically loads both files. The current branch therefore
defines the default firmware behavior.

## Daily Commands

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

## Migration Note

Legacy `SCENARIO=...` files still exist as a temporary compatibility layer during
the branch split, but they are no longer the recommended workflow.
