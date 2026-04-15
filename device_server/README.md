# Device Server Workspace

This folder is the canonical workspace for the ESP32-S3 device server.

It contains:

- `scripts/device_server.py`: foreground HTTP service
- `scripts/device_server_ctl.sh`: `nohup` background launcher
- `scripts/device_server_run.sh`: foreground runner for `systemd`
- `configs/device_server.local.env.example`: runtime secret template
- `systemd/livekit-device-server.service`: production service unit

This is now the only supported server workspace in the repository.

## Quick Start

Create the runtime env file:

```bash
cp device_server/configs/device_server.local.env.example \
   device_server/configs/device_server.local.env
```

Fill in at least:

```env
LIVEKIT_URL=wss://your-project.livekit.cloud
LIVEKIT_API_KEY=API...
LIVEKIT_API_SECRET=secret...
LIVEKIT_ROOM=lichuang-room
LIVEKIT_AGENT_NAME=my-agent
```

The firmware still points at the legacy auth endpoint:

```env
TOKEN_SERVER_URL=http://YOUR_SERVER_IP:8790/token
```

That URL is served by `device_server`.

Run in the foreground:

```bash
python3 device_server/scripts/device_server.py \
  --env-file device_server/configs/device_server.local.env
```

Run with the bundled background launcher:

```bash
bash device_server/scripts/device_server_ctl.sh start
bash device_server/scripts/device_server_ctl.sh status
bash device_server/scripts/device_server_ctl.sh logs
```

Runtime files are written under:

- `device_server/.run/device_server.log`
- `device_server/.run/device_server.pid`
- `device_server/.run/device_server/`

## systemd

1. Edit `device_server/systemd/livekit-device-server.service`.
2. Update `User`, `Group`, `WorkingDirectory`, `EnvironmentFile`,
   `ExecStart`, and `ReadWritePaths`.
3. Keep the workspace under `/opt` or `/srv`. Do not point the unit at
   `/root/...` while `ProtectHome=true` is enabled.
4. Install it:

```bash
sudo cp device_server/systemd/livekit-device-server.service \
  /etc/systemd/system/livekit-device-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now livekit-device-server
```

Useful commands:

```bash
sudo systemctl status livekit-device-server
sudo journalctl -u livekit-device-server -f
sudo systemctl restart livekit-device-server
```

Primary service env names:

- `DEVICE_SERVER_HOST`
- `DEVICE_SERVER_PORT`
- `DEVICE_SERVER_TTL_SECONDS`
- `DEVICE_SERVER_PUBLIC_HOST`
- `DEVICE_SERVER_DATA_DIR`

## API Endpoints

- `GET /healthz`
- `POST /token`
- `POST /v1/auth/token`
- `POST /v1/diagnostics/events`
- `POST /v1/diagnostics/blobs`
- `GET /v1/admin/storage`
