# Release Token Firmware

## Purpose

`firmware/release-token` is the production-oriented firmware branch for `lichuang_esp32s3`.

It keeps the LiveKit API secret on the server side only:

- the token server signs JWTs
- the device fetches a short-lived token
- the device joins LiveKit with that token
- if the token is rejected or expired, the device fetches a fresh token and retries
- after repeated auth failures, the device shows `AUTH EXPIRED`

This is the recommended release path for devices you plan to ship or deploy outside your development desk.

## Branch Defaults

- `configs/branch.defaults.env`

## What It Enables

- `PROFILE=prod`
- `AUTH_MODE=token_server`
- `START_IN_STANDBY=1`
- no debug uplink websocket export
- no debug downlink websocket export
- no local debug WAV export

## Required Local Or Server Config

Set device-side values in `configs/livekit.local.env`:

```env
TOKEN_SERVER_URL=http://YOUR_SERVER_IP:8790/token
TOKEN_SERVER_TIMEOUT_MS=5000
TOKEN_SERVER_RETRY_DELAY_MS=3000
TOKEN_SERVER_AUTH_MAX_FAILURES=3

LIVEKIT_ROOM=lichuang-room
LIVEKIT_PARTICIPANT=lichuang-esp32s3
LIVEKIT_PARTICIPANT_IDENTITY=lichuang-esp32s3
LIVEKIT_AGENT_NAME=my-agent
```

Set token-server runtime secrets in `configs/token_server.local.env`:

```env
LIVEKIT_URL=wss://your-project.livekit.cloud
LIVEKIT_API_KEY=API...
LIVEKIT_API_SECRET=secret...

LIVEKIT_ROOM=lichuang-room
LIVEKIT_AGENT_NAME=my-agent
```

Both real files stay gitignored.

## Token Server

Run the token server on a reachable machine or server:

```bash
cp configs/token_server.local.env.example configs/token_server.local.env
python3 scripts/token_server.py --env-file configs/token_server.local.env
```

Or use the bundled background launcher:

```bash
bash scripts/token_server_ctl.sh start
```

Health check:

```bash
curl http://YOUR_SERVER_IP:8790/healthz
```

## Device Behavior

### Normal path

1. Device boots
2. Device connects to Wi-Fi
3. Device enters standby and waits on the `BOOT` button
4. User presses `BOOT`
5. Device syncs clock and initializes audio
6. Device requests a token from the token server
7. Device joins the LiveKit room

If the board looks idle right after boot, that is expected for `release-token`: it will not try to join a room until `BOOT` is pressed.

### Expired or invalid token

1. LiveKit rejects the token with `BAD_TOKEN` or `UNAUTHORIZED`
2. Device requests a fresh token from the token server
3. Device retries room join
4. If repeated auth failures reach the configured limit, the screen shows `AUTH EXPIRED`

### Token server temporary failure

- the device keeps retrying token fetch with the configured retry delay
- the screen shows a retry status instead of embedding a secret into the firmware

## Build And Flash

```bash
git switch firmware/release-token
bash scripts/project.sh flash-monitor
```

## Package A Release Artifact

```bash
git switch firmware/release-token
bash scripts/package_firmware.sh
```

## Why This Is The Default Auth Path

- API secret stays off the device
- token lifetime is controlled server-side
- invalid or expired device auth can be revoked without reflashing every board
- release firmware stays closer to production security practice

## Related Docs

- `README.md`
- `README.zh-CN.md`
- `docs/profiles.md`
- `docs/firmware-packaging.md`
