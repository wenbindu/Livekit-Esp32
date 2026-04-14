# Device Server

## Purpose

The canonical workspace now lives under `device_server/`.

The root paths remain as compatibility wrappers:

- `scripts/token_server.py`
- `scripts/token_server_ctl.sh`
- `configs/token_server.local.env.example`

The real implementation now lives in:

- `device_server/scripts/device_server.py`
- `device_server/scripts/device_server_ctl.sh`
- `device_server/scripts/device_server_run.sh`
- `device_server/configs/device_server.local.env.example`
- `device_server/systemd/livekit-device-server.service`

This keeps the server deployment assets in one place while preserving backward
compatibility for existing commands and firmware docs.

The service acts as the first `device server` baseline instead of only being a
single token endpoint.

It keeps backward compatibility with the existing firmware path:

- legacy token API: `POST /token`

It also exposes separated endpoints for the long-term production shape described
in `docs/firmware-lifecycle-design.md`:

- auth API: `POST /v1/auth/token`
- diagnostics event ingest: `POST /v1/diagnostics/events`
- diagnostics blob ingest: `POST /v1/diagnostics/blobs`
- admin storage summary: `GET /v1/admin/storage`
- health: `GET /healthz`

## Why This Shape

The service is meant to evolve with production firmware:

- room auth stays isolated from diagnostics uploads
- small reboot and breadcrumb events can be ingested as JSON
- larger crash artifacts can be uploaded as blobs
- the same deployment can later support token issuance and incident diagnosis

## Auth API

Compatible endpoints:

- `POST /token`
- `POST /v1/auth/token`

Request body:

```json
{
  "room_name": "lichuang-room",
  "participant_identity": "lichuang-esp32s3",
  "participant_name": "lichuang-esp32s3",
  "participant_metadata": "{\"hw\":\"esp32s3\"}",
  "agent_name": "my-agent",
  "agent_metadata": "{\"env\":\"prod\"}"
}
```

Response body:

```json
{
  "server_url": "wss://your-project.livekit.cloud",
  "token": "eyJ...",
  "expires_at": 1712345678,
  "room_name": "lichuang-room",
  "participant_identity": "lichuang-esp32s3"
}
```

## Diagnostics Event Ingest

Endpoint:

- `POST /v1/diagnostics/events`

Accepted request shapes:

1. Single event object
2. Array of event objects
3. Object with shared context plus `events`

Example batch:

```json
{
  "device_id": "lichuang-esp32s3",
  "participant_identity": "lichuang-esp32s3",
  "events": [
    {
      "type": "boot_summary",
      "session_id": 32,
      "boot_count": 32,
      "reboot_streak": 0,
      "reset_reason": "panic",
      "previous_breadcrumb": "token_fetch"
    },
    {
      "type": "room_connected",
      "session_id": 32,
      "breadcrumb": "room_connected"
    }
  ]
}
```

Storage:

- JSON Lines files under `device_server/.run/device_server/events/YYYYMMDD/<device>.jsonl`

## Diagnostics Blob Ingest

Endpoint:

- `POST /v1/diagnostics/blobs`

Two supported request styles:

1. Raw binary body with metadata in query or headers
2. JSON body with `content_b64`

Raw upload example:

```bash
curl -X POST \
  "http://127.0.0.1:8790/v1/diagnostics/blobs?device_id=lichuang-esp32s3&kind=coredump&filename=coredump.elf&session_id=32" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @coredump.elf
```

JSON upload example:

```json
{
  "device_id": "lichuang-esp32s3",
  "kind": "boot-log",
  "filename": "boot.log",
  "session_id": "32",
  "content_b64": "SGVsbG8K",
  "metadata": {
    "compression": "none"
  }
}
```

Storage:

- blob files under `device_server/.run/device_server/blobs/YYYYMMDD/<device>/`
- sidecar metadata JSON next to each blob file

Each stored blob records:

- `device_id`
- `kind`
- `session_id`
- `boot_count`
- `reboot_streak`
- request headers used for ingest
- `sha256`
- byte size

## Admin Summary

Endpoint:

- `GET /v1/admin/storage`

Response includes:

- event file count and total bytes
- blob file count and total bytes
- latest modification timestamp for each tree

## Workspace Layout

- `device_server/scripts/device_server.py`: foreground server
- `device_server/scripts/device_server_ctl.sh`: background launcher
- `device_server/scripts/device_server_run.sh`: `systemd` foreground runner
- `device_server/configs/device_server.local.env`: runtime env file
- `device_server/systemd/livekit-device-server.service`: deployment unit

## Runtime Config

The canonical launcher is now `device_server/scripts/device_server_ctl.sh`.

For old deployments, `scripts/token_server.py` and
`scripts/token_server_ctl.sh` still work and forward to the workspace.

Useful env vars:

- `TOKEN_SERVER_HOST`
- `TOKEN_SERVER_PORT`
- `TOKEN_SERVER_HTTP_PATH`
- `TOKEN_SERVER_TTL_SECONDS`
- `TOKEN_SERVER_PUBLIC_HOST`
- `DEVICE_SERVER_ENV_FILE`
- `DEVICE_SERVER_AUTH_PATH`
- `DEVICE_SERVER_EVENT_PATH`
- `DEVICE_SERVER_BLOB_PATH`
- `DEVICE_SERVER_ADMIN_PATH`
- `DEVICE_SERVER_DATA_DIR`
- `DEVICE_SERVER_RUN_DIR`
- `DEVICE_SERVER_PID_FILE`
- `DEVICE_SERVER_LOG_FILE`
- `DEVICE_SERVER_MAX_EVENT_BYTES`
- `DEVICE_SERVER_MAX_BLOB_BYTES`

## Deployment

For manual background launch:

```bash
cp device_server/configs/device_server.local.env.example \
   device_server/configs/device_server.local.env
bash device_server/scripts/device_server_ctl.sh start
```

For `systemd`, edit and install:

```bash
sudo cp device_server/systemd/livekit-device-server.service \
  /etc/systemd/system/livekit-device-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now livekit-device-server
```

More deployment notes:

- `device_server/README.md`

## Current Limits

Default body limits:

- event payload: `64KB`
- blob payload: `1MB`

These defaults are intended for:

- compact reboot and breadcrumb uploads
- compressed coredump or crash artifacts from ESP32-S3

## Related Docs

- `docs/release-token.md`
- `docs/production-diagnostics.md`
- `docs/firmware-lifecycle-design.md`
- `device_server/README.md`
