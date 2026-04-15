# Device Server

`device_server/` is the only supported server workspace in this repository.

It contains:

- `scripts/device_server.py`: foreground HTTP service
- `scripts/device_server_ctl.sh`: `nohup` background launcher
- `scripts/device_server_run.sh`: foreground runner for `systemd`
- `configs/device_server.local.env.example`: runtime env template
- `systemd/livekit-device-server.service`: production service unit

## Fixed Endpoints

The service paths are fixed and are not configured by env anymore:

- `GET /healthz`
- `POST /token`
- `POST /v1/auth/token`
- `POST /v1/diagnostics/events`
- `POST /v1/diagnostics/blobs`
- `GET /v1/admin/storage`

The firmware should still point to the legacy auth endpoint:

```env
TOKEN_SERVER_URL=http://YOUR_SERVER_IP:8790/token
```

When `agent_name` is present in the request or `LIVEKIT_AGENT_NAME` is set in
the server env, `/token` now also ensures a LiveKit agent dispatch for that
room. If dispatch fails, the token request fails instead of leaving the device
connected without an AI participant.

## Runtime Env

Create the runtime env file:

```bash
cp device_server/configs/device_server.local.env.example \
  device_server/configs/device_server.local.env
```

Minimum required values:

```env
LIVEKIT_URL=wss://your-project.livekit.cloud
LIVEKIT_API_KEY=API...
LIVEKIT_API_SECRET=secret...
```

Recommended full example:

```env
LIVEKIT_URL=wss://raingo-1yscbcrq.livekit.cloud
LIVEKIT_API_KEY=API...
LIVEKIT_API_SECRET=secret...

LIVEKIT_ROOM=lichuang-room
LIVEKIT_AGENT_NAME=Raingo

DEVICE_SERVER_HOST=0.0.0.0
DEVICE_SERVER_PORT=8790
DEVICE_SERVER_TTL_SECONDS=3600
DEVICE_SERVER_PUBLIC_HOST=115.29.195.55
DEVICE_SERVER_DATA_DIR=/opt/livekit-esp32s3/device_server/.run/device_server

# Optional
# DEVICE_SERVER_MAX_EVENT_BYTES=65536
# DEVICE_SERVER_MAX_BLOB_BYTES=1048576
```

Primary runtime env names:

- `DEVICE_SERVER_HOST`
- `DEVICE_SERVER_PORT`
- `DEVICE_SERVER_TTL_SECONDS`
- `DEVICE_SERVER_PUBLIC_HOST`
- `DEVICE_SERVER_DATA_DIR`
- `DEVICE_SERVER_MAX_EVENT_BYTES`
- `DEVICE_SERVER_MAX_BLOB_BYTES`

Legacy aliases still accepted:

- `TOKEN_SERVER_HOST`
- `TOKEN_SERVER_PORT`
- `TOKEN_SERVER_TTL_SECONDS`
- `TOKEN_SERVER_PUBLIC_HOST`
- `TOKEN_SERVER_ENV_FILE`

## Python And venv

The launcher prefers the repository-root Python venv:

- `${REPO_ROOT}/.venv/bin/python3`

If the system `python3` is too old, create a newer venv in the repository root:

```bash
cd /opt/livekit-esp32s3
python3.13 -m venv .venv
.venv/bin/python --version
.venv/bin/python device_server/scripts/device_server.py --help
```

The `systemd` service and shell launchers will automatically use that venv.

## Manual Run

Foreground run:

```bash
python3 device_server/scripts/device_server.py \
  --env-file device_server/configs/device_server.local.env
```

Background run:

```bash
bash device_server/scripts/device_server_ctl.sh start
bash device_server/scripts/device_server_ctl.sh status
bash device_server/scripts/device_server_ctl.sh logs
```

Runtime files are written under:

- `device_server/.run/device_server.log`
- `device_server/.run/device_server.pid`
- `device_server/.run/device_server/`

## systemd Deployment

Recommended deployment path:

- `/opt/livekit-esp32s3`
- or `/srv/livekit-esp32s3`

Do not point the service at `/root/...` while `ProtectHome=true` is enabled.

### 1. Sync the repo

```bash
cd /opt/livekit-esp32s3
git switch dev
git pull origin dev
mkdir -p device_server/.run
```

### 2. Create the runtime env

```bash
cat > /opt/livekit-esp32s3/device_server/configs/device_server.local.env <<'EOF'
LIVEKIT_URL=wss://your-project.livekit.cloud
LIVEKIT_API_KEY=API...
LIVEKIT_API_SECRET=secret...

LIVEKIT_ROOM=lichuang-room
LIVEKIT_AGENT_NAME=my-agent

DEVICE_SERVER_HOST=0.0.0.0
DEVICE_SERVER_PORT=8790
DEVICE_SERVER_TTL_SECONDS=3600
DEVICE_SERVER_PUBLIC_HOST=YOUR_PUBLIC_IP
DEVICE_SERVER_DATA_DIR=/opt/livekit-esp32s3/device_server/.run/device_server
EOF
```

### 3. Create the root venv

```bash
cd /opt/livekit-esp32s3
python3.13 -m venv .venv
.venv/bin/python --version
```

### 4. Install the unit

Edit `device_server/systemd/livekit-device-server.service` first, then install:

```bash
sudo cp device_server/systemd/livekit-device-server.service \
  /etc/systemd/system/livekit-device-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now livekit-device-server
```

The important fields to customize are:

- `User`
- `Group`
- `WorkingDirectory`
- `EnvironmentFile`
- `ExecStart`
- `ReadWritePaths`

### 5. Check the service

```bash
sudo systemctl status livekit-device-server -l --no-pager
sudo journalctl -u livekit-device-server -f
```

Useful restart commands:

```bash
sudo systemctl restart livekit-device-server
sudo systemctl stop livekit-device-server
sudo systemctl start livekit-device-server
```

## curl Tests

### Health Check

Local:

```bash
curl -sS http://127.0.0.1:8790/healthz
```

Public:

```bash
curl -sS http://YOUR_PUBLIC_IP:8790/healthz
```

Expected:

```json
{"ok":true,...}
```

### Legacy Token API

```bash
curl -sS -X POST http://127.0.0.1:8790/token \
  -H 'Content-Type: application/json' \
  --data '{
    "room_name":"lichuang-room",
    "participant_identity":"curl-test-device",
    "participant_name":"curl-test-device"
  }'
```

Expected fields:

- `server_url`
- `token`
- `expires_at`
- `room_name`
- `participant_identity`

### New Auth API

```bash
curl -sS -X POST http://127.0.0.1:8790/v1/auth/token \
  -H 'Content-Type: application/json' \
  --data '{
    "room_name":"lichuang-room",
    "participant_identity":"curl-auth-device",
    "participant_name":"curl-auth-device"
  }'
```

### Diagnostics Event Upload

```bash
curl -sS -X POST http://127.0.0.1:8790/v1/diagnostics/events \
  -H 'Content-Type: application/json' \
  --data '{
    "device_id":"curl-test-device",
    "type":"boot_summary",
    "session_id":1,
    "boot_count":1,
    "reboot_streak":0
  }'
```

Expected fields:

- `ok`
- `batch_id`
- `accepted`
- `paths`

### Diagnostics Blob Upload

Raw WAV upload example:

```bash
curl -sS -X POST http://127.0.0.1:8790/v1/diagnostics/blobs \
  -H 'Content-Type: audio/wav' \
  -H 'X-Device-Id: curl-test-device' \
  -H 'X-Diag-Kind: downlink_audio' \
  -H 'X-File-Name: downlink_s1_b1_seg001.wav' \
  -H 'X-Session-Id: 1' \
  -H 'X-Boot-Count: 1' \
  -H 'X-Reboot-Streak: 0' \
  --data-binary @downlink.wav
```

Current firmware uses this endpoint to upload rendered downlink audio before
speaker playback when built with `FIRMWARE_PRESET=downlink-http`.

Stored files land under:

- `device_server/.run/device_server/blobs/YYYYMMDD/<device>/`

### Storage Summary

```bash
curl -sS http://127.0.0.1:8790/v1/admin/storage
```

## Troubleshooting

If `systemd` fails immediately, check these first:

1. `WorkingDirectory`, `EnvironmentFile`, `ExecStart`, and `ReadWritePaths` all point to the same real path.
2. The service user can read the repo and write `device_server/.run`.
3. The repository root contains a usable `.venv/bin/python3`, or the system `python3` is new enough.
4. The port is not already occupied.

Useful commands:

```bash
sudo systemctl status livekit-device-server -l --no-pager
sudo journalctl -u livekit-device-server -n 100 --no-pager
ls -ld /opt/livekit-esp32s3 /opt/livekit-esp32s3/device_server /opt/livekit-esp32s3/device_server/.run
lsof -nP -iTCP:8790 -sTCP:LISTEN
```
