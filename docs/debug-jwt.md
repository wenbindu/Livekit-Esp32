# Debug JWT Firmware

## Purpose

`debug-jwt` is the lightweight development scenario for cases where the board cannot reach a token server but you still need to test LiveKit room connectivity from the device.

Scenario file:

- `configs/scenarios/debug-jwt.env`

## What It Changes

`debug-jwt` keeps the normal chat flow enabled but removes the extra debug-audio websocket export.

It is intended to be lighter than `dev-audio-ws`.

Enabled behavior:

- `PROFILE=dev`
- `AUTH_MODE=device_jwt`
- `ENABLE_DEBUG_UPLINK_WS=0`
- `ENABLE_DEBUG_DOWNLINK_WS=0`
- `ENABLE_DEBUG_UPLINK_WAV=0`
- immediate join flow

## Security Warning

This mode is for development only.

Because `AUTH_MODE=device_jwt` is used, the firmware build embeds:

- `LIVEKIT_URL`
- `LIVEKIT_API_KEY`
- `LIVEKIT_API_SECRET`

Do not distribute this firmware as a production image.

Prefer `AUTH_MODE=token_server` for normal development and production.

## Required Local Config

Create the local file first:

```bash
cp configs/livekit.local.env.example configs/livekit.local.env
```

Then set at least:

```env
AUTH_MODE=device_jwt
LIVEKIT_URL=wss://your-project.livekit.cloud
LIVEKIT_API_KEY=API...
LIVEKIT_API_SECRET=secret...
LIVEKIT_JWT_TTL_SECONDS=3600
LIVEKIT_ROOM=lichuang-room
LIVEKIT_PARTICIPANT=lichuang-esp32s3
LIVEKIT_PARTICIPANT_IDENTITY=lichuang-esp32s3
LIVEKIT_AGENT_NAME=my-agent
```

The real `configs/livekit.local.env` file is gitignored.

## Build And Flash

Build:

```bash
SCENARIO=debug-jwt bash scripts/project.sh build
```

Flash and monitor:

```bash
SCENARIO=debug-jwt bash scripts/project.sh flash-monitor
```

If you only need serial logs:

```bash
SCENARIO=debug-jwt bash scripts/project.sh monitor
```

## Package A Reusable Artifact

```bash
bash scripts/package_firmware.sh debug-jwt
```

## When To Use It

Use `debug-jwt` when:

- the board cannot reach your local or remote token server
- you want fewer local debug tasks than `dev-audio-ws`
- you want to focus on room connection, auth, and basic voice flow first

Do not use it when:

- you need dual-path WAV capture
- you want production-like security
- you are preparing a firmware image to share broadly

## Difference From Other Scenarios

### Compared with `dev-chat`

- auth mode switches to `device_jwt`
- intended for environments where token server reachability is the main blocker

### Compared with `dev-audio-ws`

- debug websocket export is disabled
- no desktop WAV capture
- lower local debug overhead

## Lichuang Board Notes

For the current Lichuang board setup:

- target remains `esp32s3`
- serial port is typically exposed over USB Type-C
- playback stays on the mono application path used by the single-speaker hardware
- BOOT button behavior and Wi-Fi provisioning flow stay unchanged

## Troubleshooting

### `JWT BUILD FAILED`

Check:

- system time is valid
- `LIVEKIT_API_KEY` and `LIVEKIT_API_SECRET` are set correctly
- the board has network access before room join

### `401` or auth errors

Check:

- `LIVEKIT_URL`
- `LIVEKIT_API_KEY`
- `LIVEKIT_API_SECRET`
- device clock drift

### Board connects but there is still no conversation

Check in this order:

1. whether the web client can talk to the same agent
2. whether the agent worker is healthy
3. whether STT, LLM, and TTS providers are configured
4. whether the agent is actually subscribing to the room audio track

If the web client also fails, stop changing firmware first and inspect the agent logs.

## Related Docs

- `README.md`
- `README.zh-CN.md`
- `docs/profiles.md`
- `docs/firmware-packaging.md`
- `docs/debug-audio.md`
