# Debug Audio Workflow

## Purpose

Use this workflow when you need to inspect both sides of device audio:

- processed user uplink audio
- rendered AI downlink audio

This is a development-time diagnostic path. It is no longer a long-lived branch.

## Recommended Debug Firmware

Use the `dev` branch with the `audio-trace` preset:

```bash
git switch dev
FIRMWARE_PRESET=audio-trace bash scripts/project.sh flash-monitor
```

If you want a reusable diagnostic artifact, package it with:

```bash
git switch dev
bash scripts/package_firmware.sh preset audio-trace
```

If you only need processed uplink audio, use:

```bash
git switch dev
FIRMWARE_PRESET=uplink-trace bash scripts/project.sh flash-monitor
```

## What Gets Captured

### Uplink

- stream name: `uplink`
- format: `pcm_s16le`
- sample rate: `16000`
- channels: `1`
- source: processed microphone path exported from the device capture pipeline
- position in pipeline: PCM after device-side voice processing, before network upload encoding

### Downlink

- stream name: `downlink`
- format: `pcm_s16le`
- sample rate: `16000`
- channels: `1`
- source: rendered AI audio before speaker playback
- position in pipeline: PCM headed into the render/playback path

## Config Layers

Branch-owned defaults:

- `configs/branch.defaults.env` on `dev`

Diagnostic preset:

- `configs/presets/audio-trace.env`

Local endpoints:

- `configs/livekit.local.env`

The preset enables:

- `ENABLE_DEBUG_UPLINK_WS=1`
- `ENABLE_DEBUG_DOWNLINK_WS=1`
- normal chat flow remains enabled

## Local Config

Set your Mac or workstation LAN IP in:

- `configs/livekit.local.env`

Required keys:

```env
DEBUG_UPLINK_WS_URL=ws://YOUR_MAC_IP:8765/uplink
DEBUG_DOWNLINK_WS_URL=ws://YOUR_MAC_IP:8765/downlink
```

If the board changes Wi-Fi networks, update these URLs to the new reachable
host IP before reflashing.

## Start The Receiver

Recommended command from the project root:

```bash
bash scripts/debug_audio_ws_start.sh
```

Stop it with:

```bash
bash scripts/debug_audio_ws_stop.sh
```

The start script launches `scripts/debug_uplink_ws_server.py` in the background
and writes:

- PID file: `.run/debug_audio_ws.pid`
- log file: `.run/debug_audio_ws.log`
- WAV output: `debug_audio_ws/`

If you want to run the receiver manually, the equivalent command is:

```bash
python3 scripts/debug_uplink_ws_server.py \
  --host 0.0.0.0 \
  --port 8765 \
  --path /uplink \
  --path /downlink \
  --output-dir ./debug_audio_ws \
  --session-idle-seconds 20
```

## Practical Workflow

1. Start the local WS receiver with `bash scripts/debug_audio_ws_start.sh`.
2. Switch to `dev`.
3. Flash with `FIRMWARE_PRESET=audio-trace`.
4. Wait for both WebSocket connections.
5. Speak to the device for 20 to 30 seconds.
6. Trigger an AI response long enough to exercise playback.
7. Inspect the newest same-timestamp `uplink_*.wav` and `downlink_*.wav`.

## Merge Rule

When you find a bug while using `audio-trace`:

1. separate diagnostic-only changes from functional fixes
2. merge functional fixes back to `dev`
3. promote `dev -> test -> main`

Do not keep permanent functional fixes only inside a trace-only workflow.

## Troubleshooting

### No WebSocket connection

Check:

- Mac and device are on reachable networks
- `DEBUG_UPLINK_WS_URL` and `DEBUG_DOWNLINK_WS_URL` point to the current host IP
- local firewall is not blocking port `8765`

### Frequent reconnects

Check:

- LiveKit room stability
- Wi-Fi quality
- debug WebSocket host reachability

## Related Docs

- `docs/branch-workflow.md`
- `docs/firmware-packaging.md`
- `docs/firmware-lifecycle-design.md`
