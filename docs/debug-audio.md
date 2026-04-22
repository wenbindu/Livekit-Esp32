# Debug Audio Workflow

## Purpose

Use this workflow when you need to inspect both sides of device audio:

- processed user uplink audio
- rendered AI downlink audio

This is a development-time diagnostic path. It is no longer a long-lived branch.

There are now two downlink capture paths:

- `audio-trace`: export uplink/downlink PCM over local WebSocket to a workstation
- `downlink-http`: stage rendered downlink WAV segments on the device and upload
  them to `device_server` over HTTP

## Recommended Debug Firmware

Use the `dev` branch with the preset that matches the problem:

- `audio-trace` when you need simultaneous uplink and downlink PCM on a workstation
- `downlink-http` when you mainly need rendered downlink WAV artifacts on `device_server`

WebSocket bidirectional trace example:

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

If you want downlink audio to land directly on the deployed `device_server`,
use:

```bash
git switch dev
FIRMWARE_PRESET=downlink-http bash scripts/project.sh flash-monitor
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

That means:

- if the saved downlink WAV already contains crackle, the issue is upstream of
  the speaker path
- if the saved downlink WAV is clean but the speaker sounds bad, the issue is
  in render FIFO, I2S, codec, gain, or board-side playback

## Config Layers

Branch-owned defaults:

- `configs/branch.defaults.env` on `dev`

Diagnostic preset:

- `configs/presets/audio-trace.env`
- `configs/presets/downlink-http.env`

Local endpoints:

- `configs/livekit.local.env`

Preset behavior:

- `audio-trace`: `ENABLE_DEBUG_UPLINK_WS=1`, `ENABLE_DEBUG_DOWNLINK_WS=1`
- `uplink-trace`: `ENABLE_DEBUG_UPLINK_WS=1`
- `downlink-http`: `ENABLE_DEBUG_DOWNLINK_HTTP_UPLOAD=1`
- normal chat flow remains enabled in all three presets

## Local Config

Set your Mac or workstation LAN IP in:

- `configs/livekit.local.env`

Required keys for `audio-trace`:

```env
DEBUG_UPLINK_WS_URL=ws://YOUR_MAC_IP:8765/uplink
DEBUG_DOWNLINK_WS_URL=ws://YOUR_MAC_IP:8765/downlink
```

Optional keys for `downlink-http`:

```env
DEBUG_DOWNLINK_HTTP_RINGBUF_KB=24
DEBUG_DOWNLINK_HTTP_IDLE_FLUSH_MS=1000
DEBUG_DOWNLINK_HTTP_SEGMENT_SECONDS=8
```

If the board changes Wi-Fi networks and you use `audio-trace`, update these
URLs to the new reachable host IP before reflashing.

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

## HTTP Upload Workflow

Use this when the board cannot reach a workstation WebSocket receiver reliably
but can reach `device_server`.

1. Confirm `TOKEN_SERVER_URL` points at the deployed `device_server`.
2. Flash with `FIRMWARE_PRESET=downlink-http`.
3. Trigger an AI response long enough to reproduce the playback crackle.
4. Wait about `DEBUG_DOWNLINK_HTTP_IDLE_FLUSH_MS` after playback stops.
5. Inspect `device_server/.run/device_server/blobs/YYYYMMDD/<device>/`.

Each uploaded blob is:

- kind: `downlink_audio`
- file type: `.wav`
- capture point: rendered PCM before speaker playback

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
