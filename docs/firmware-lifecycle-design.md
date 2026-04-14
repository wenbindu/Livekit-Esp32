# Firmware Lifecycle Design

## Goal

Replace the old long-lived firmware-variant branch model with:

- lifecycle branches: `dev`, `test`, `main`
- diagnostic presets: temporary overlays such as `uplink-trace` and `audio-trace`

This keeps business logic moving through a normal promotion path while keeping
debug tooling available when needed.

## Why The Old Model Was Failing

The old model mixed two separate concerns into git branches:

- lifecycle state: development vs release
- diagnostic behavior: uplink trace, downlink trace, standby-first flow

That caused branch drift. For example, `dev` already contains
`8c3e452 improve livekit reconnect recovery`, while the old `fw-*` branches do
not. That means a bug can be fixed in one path and remain broken in another
path even though both are supposed to represent the same firmware family.

## New Model

### Lifecycle Branches

- `dev`
  Main development branch. Fast iteration. Default profile `dev`.
- `test`
  Integration and regression branch. Behavior close to production. Default
  profile `test`.
- `main`
  Production baseline. Stable token-server firmware. Default profile `prod`.

### Diagnostic Presets

Diagnostic presets are optional overlays loaded after the branch defaults:

- `uplink-trace`
  Export processed user uplink audio over WebSocket.
- `audio-trace`
  Export both processed uplink audio and rendered downlink audio over
  WebSocket.

Presets are not long-lived product branches. They are temporary diagnostics.

## Config Layers

Firmware behavior now comes from four layers:

1. `configs/sdkconfig.defaults.board.<board>`
2. `configs/sdkconfig.defaults.<profile>`
3. `configs/branch.defaults.env`
4. `configs/presets/<preset>.env` when `FIRMWARE_PRESET` is set
5. `configs/livekit.local.env` for machine-local endpoints and secrets

Practical meaning:

- branch controls lifecycle intent
- preset controls temporary diagnostics
- local env controls machine-local addresses and secrets

## Branch Ownership Rules

Each lifecycle branch owns only its default lifecycle behavior:

- `dev`: token server auth, no standby, no debug audio by default
- `test`: token server auth, standby-first, no debug audio by default
- `main`: token server auth, standby-first, no debug audio by default

Branch defaults must stay minimal. If a new need is temporary or
investigation-only, it belongs in a preset, not in a long-lived branch.

## Merge Policy

All functional work follows:

1. develop on `dev`
2. promote to `test` for validation
3. merge to `main` after acceptance

When debugging with audio trace:

1. start from `dev`
2. enable a preset such as `audio-trace`
3. make small diagnostic commits separately from functional fixes
4. merge functional fixes back to `dev`
5. keep diagnostic-only changes only if they are reusable and default-off

Do not keep production fixes trapped inside a debug-only branch.

## Packaging Rule

Packaging is branch-first and preset-second:

- package `dev`, `test`, or `main` directly for normal artifacts
- add `preset <name>` only when you intentionally want a diagnostic artifact

Examples:

```bash
git switch dev
bash scripts/package_firmware.sh

git switch dev
bash scripts/package_firmware.sh preset audio-trace

git switch test
bash scripts/package_firmware.sh

git switch main
bash scripts/package_firmware.sh
```

## Production Diagnostics Direction

The production branch should not carry development-only outputs such as
WebSocket audio export or local debug WAV endpoints. It should carry a stable
diagnostics baseline instead:

- reset reason capture
- boot count and reboot streak
- lifecycle breadcrumb before risky phases
- core dump to flash
- deferred upload after next successful boot
- compact event upload to the server

This keeps production firmware observable without keeping heavy debug features
permanently enabled.

## Token Server Direction

The current token server can remain the first module, but the long-term service
shape should become a device service with separate concerns:

- auth API for token issuance
- diagnostics event ingest
- crash/blob ingest for larger uploads

That allows the same deployment to support both room auth and production
incident diagnosis without coupling everything into one endpoint.

## Migration Plan

1. Move long-lived firmware branches to `dev`, `test`, `main`.
2. Convert old debug branches into preset files.
3. Update packaging and build scripts to understand presets.
4. Archive or delete the old `fw-*` branches after migration.
5. Add production diagnostics features on top of the new lifecycle model.
