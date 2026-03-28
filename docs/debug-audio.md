# Debug Audio Workflow

## Purpose

Use this workflow when you need to inspect both sides of device audio:

- processed user uplink audio
- rendered AI downlink audio

This is the recommended development-time audio debugging path for `lichuang_esp32s3`.

## Recommended Debug Firmware

Use the dedicated debug-audio scenario:

```bash
SCENARIO=dev-audio-ws
```

This firmware keeps normal chat enabled and adds two lightweight debug exports:

- processed user uplink PCM over WebSocket
- rendered AI downlink PCM over WebSocket

Build and flash it with:

```bash
SCENARIO=dev-audio-ws bash scripts/project.sh flash-monitor
```

If you want a reusable firmware drop, package it with:

```bash
bash scripts/package_firmware.sh dev-audio-ws
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

## Scenario

Use:

```bash
SCENARIO=dev-audio-ws
```

Scenario file:

- `configs/scenarios/dev-audio-ws.env`

This enables:

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

If the board changes Wi-Fi networks, update these URLs to the new reachable host IP before reflashing.

## Start The Receiver

Recommended command from the project root:

```bash
bash scripts/debug_audio_ws_start.sh
```

Stop it with:

```bash
bash scripts/debug_audio_ws_stop.sh
```

The start script launches `scripts/debug_uplink_ws_server.py` in the background and writes:

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

The receiver writes WAV files into:

- `debug_audio_ws/`

Generated files are gitignored.

## Flash And Monitor

Build, flash, and monitor with:

```bash
SCENARIO=dev-audio-ws bash scripts/project.sh flash-monitor
```

If you only need serial logs:

```bash
SCENARIO=dev-audio-ws bash scripts/project.sh monitor
```

## Expected Serial Logs

Healthy startup should include:

- `Streaming uplink PCM to ws://.../uplink`
- `Streaming downlink PCM to ws://.../downlink`
- `Debug uplink websocket connected`
- `Debug downlink websocket connected`
- `AUD_SRC: Start to fetch audio src data now`
- `Published audio frame: count=...`
- `Debug uplink ws frames=...`
- `Debug downlink ws frames=...`

If those appear, both capture directions are live.

## Output Files

Output directory:

- `debug_audio_ws/`

Typical filenames:

- `uplink_YYYYMMDD_HHMMSS.wav`
- `downlink_YYYYMMDD_HHMMSS.wav`

Pairing behavior:

- one conversation session uses one shared timestamp for both files
- short websocket reconnects append to the same pair instead of creating many tiny files
- a new timestamp starts only after the receiver sees no active stream for the idle timeout window

This means `uplink_<ts>.wav` and `downlink_<ts>.wav` are intended to be analyzed as a pair.

## Practical Workflow

1. Start the local WS receiver with `bash scripts/debug_audio_ws_start.sh`.
2. Flash `SCENARIO=dev-audio-ws`.
3. Wait for both websocket connections.
4. Speak to the device for 20 to 30 seconds.
5. Trigger an AI response long enough to exercise playback.
6. Stop the WS receiver if you are done.
7. Inspect the newest same-timestamp `uplink_*.wav` and `downlink_*.wav`.

## How To Interpret Results

If `uplink` sounds noisy or hollow:

- suspect microphone gain
- suspect AEC/reference routing
- suspect local speech enhancement settings

If `uplink` sounds clean but agent response is bad:

- suspect STT or cloud-side turn-taking

If `downlink` file sounds clean but the speaker sounds wrong:

- suspect local playback path
- suspect speaker hardware
- suspect render gain or amplifier settings

If `downlink` file already sounds wrong:

- suspect remote TTS or received audio path

## Current Known Behavior

- `uplink` is often longer than `downlink`, because the user may speak for longer than the AI responds.
- A new file pair starts after the receiver idle timeout expires, default `20s`.
- Short `44B` WAV files mean only a WAV header was written and no PCM frames arrived.
- `No model to load` for wake models does not block this debug workflow.

## Troubleshooting

### No websocket connection

Check:

- Mac and device are on reachable networks
- `DEBUG_UPLINK_WS_URL` and `DEBUG_DOWNLINK_WS_URL` point to the current host IP
- local firewall is not blocking port `8765`

### `44B` uplink WAV only

Check serial logs for:

- `AUD_SRC: Start to fetch audio src data now`
- `Published audio frame: count=...`

If these are missing, the capture path did not fully start.

### Frequent reconnects

Check:

- LiveKit room stability
- Wi-Fi quality
- debug websocket host reachability

### Files still split too often

Check:

- whether the receiver was restarted mid-session
- whether the board lost reachability to the host IP
- whether the conversation had a long silent gap larger than `--session-idle-seconds`

If needed, increase the idle window in the manual receiver command.

### Receiver start script fails immediately

Check:

- whether port `8765` is already occupied by another local service
- whether you need to override `DEBUG_AUDIO_WS_PORT` for a temporary test
- if you change the port, update `DEBUG_UPLINK_WS_URL` and `DEBUG_DOWNLINK_WS_URL` to match before flashing

## Related Docs

- `docs/firmware-packaging.md`
- `docs/profiles.md`
- `docs/rtc-debug-notes.md`
