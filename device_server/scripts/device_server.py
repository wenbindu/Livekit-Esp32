#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import re
import time
import uuid
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlsplit


DEFAULT_LEGACY_TOKEN_PATH = "/token"
DEFAULT_AUTH_PATH = "/v1/auth/token"
DEFAULT_EVENT_PATH = "/v1/diagnostics/events"
DEFAULT_BLOB_PATH = "/v1/diagnostics/blobs"
DEFAULT_ADMIN_PATH = "/v1/admin/storage"
DEFAULT_DATA_DIR = ".run/device_server"
DEFAULT_MAX_EVENT_BYTES = 64 * 1024
DEFAULT_MAX_BLOB_BYTES = 1024 * 1024


class RequestError(Exception):
    def __init__(self, status: HTTPStatus, error: str, detail: str | None = None) -> None:
        super().__init__(detail or error)
        self.status = status
        self.error = error
        self.detail = detail


@dataclass(frozen=True)
class DeviceServiceConfig:
    legacy_token_path: str
    auth_path: str
    event_path: str
    blob_path: str
    admin_path: str
    ttl_seconds: int
    data_dir: Path
    max_event_bytes: int
    max_blob_bytes: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="LiveKit device server for livekit-esp32s3")
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--env-file", default="configs/device_server.local.env")
    parser.add_argument("--ttl-seconds", type=int, default=None)
    parser.add_argument("--data-dir", default=None)
    parser.add_argument("--max-event-bytes", type=int, default=None)
    parser.add_argument("--max-blob-bytes", type=int, default=None)
    return parser.parse_args()


def load_env_file(path: str) -> None:
    env_path = Path(path)
    if not env_path.exists():
        return

    parsed_env: dict[str, str] = {}
    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        parsed_env[key] = value

    for key, value in parsed_env.items():
        os.environ.setdefault(key, value)


def env_or_default(arg_value: str | None, env_name: str, default: str) -> str:
    if arg_value is not None:
        return arg_value
    return os.environ.get(env_name, default)


def env_or_default_aliases(arg_value: str | None, env_names: tuple[str, ...], default: str) -> str:
    if arg_value is not None:
        return arg_value
    for env_name in env_names:
        raw = os.environ.get(env_name)
        if raw is not None and raw != "":
            return raw
    return default


def env_or_default_int(arg_value: int | None, env_name: str, default: int) -> int:
    if arg_value is not None:
        return arg_value
    raw = os.environ.get(env_name)
    if raw is None or raw == "":
        return default
    return int(raw)


def env_or_default_int_aliases(arg_value: int | None, env_names: tuple[str, ...], default: int) -> int:
    if arg_value is not None:
        return arg_value
    for env_name in env_names:
        raw = os.environ.get(env_name)
        if raw is not None and raw != "":
            return int(raw)
    return default


def compact_json(value: Any) -> str:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def b64url_json(value: dict[str, Any]) -> str:
    encoded = compact_json(value).encode("utf-8")
    return base64.urlsafe_b64encode(encoded).rstrip(b"=").decode("ascii")


def sign_jwt(api_key: str, api_secret: str, payload: dict[str, Any]) -> str:
    header = {"alg": "HS256", "typ": "JWT"}
    signing_input = f"{b64url_json(header)}.{b64url_json(payload)}"
    signature = hmac.new(api_secret.encode("utf-8"), signing_input.encode("utf-8"), hashlib.sha256).digest()
    encoded_signature = base64.urlsafe_b64encode(signature).rstrip(b"=").decode("ascii")
    return f"{signing_input}.{encoded_signature}"


def build_access_token(request: dict[str, Any], ttl_seconds: int) -> dict[str, Any]:
    api_key = os.environ.get("LIVEKIT_API_KEY", "")
    api_secret = os.environ.get("LIVEKIT_API_SECRET", "")
    server_url = os.environ.get("LIVEKIT_URL", "")

    if not api_key or not api_secret or not server_url:
        raise RuntimeError("LIVEKIT_URL, LIVEKIT_API_KEY, and LIVEKIT_API_SECRET are required")

    room_name = request.get("room_name") or os.environ.get("LIVEKIT_ROOM", "lichuang-room")
    participant_identity = request.get("participant_identity") or request.get("participant_name") or "esp32s3-device"
    participant_name = request.get("participant_name") or participant_identity
    participant_metadata = request.get("participant_metadata")
    agent_name = request.get("agent_name") or os.environ.get("LIVEKIT_AGENT_NAME")
    agent_metadata = request.get("agent_metadata") or os.environ.get("LIVEKIT_AGENT_METADATA")

    now = int(time.time())
    payload: dict[str, Any] = {
        "iss": api_key,
        "sub": participant_identity,
        "nbf": now,
        "exp": now + ttl_seconds,
        "name": participant_name,
        "video": {
            "roomJoin": True,
            "room": room_name,
            "canPublish": True,
            "canSubscribe": True,
            "canPublishData": True,
        },
    }

    if participant_metadata:
        payload["metadata"] = participant_metadata

    if agent_name:
        payload["roomConfig"] = {
            "agents": [
                {
                    "agentName": agent_name,
                    **({"metadata": agent_metadata} if agent_metadata else {}),
                }
            ]
        }

    token = sign_jwt(api_key, api_secret, payload)
    return {
        "server_url": server_url,
        "token": token,
        "expires_at": now + ttl_seconds,
        "room_name": room_name,
        "participant_identity": participant_identity,
    }


def sanitize_segment(value: str | None, fallback: str) -> str:
    if value is None:
        return fallback
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", value.strip())
    cleaned = cleaned.strip("._-")
    if not cleaned:
        return fallback
    return cleaned[:96]


def first_non_empty(*values: Any) -> str | None:
    for value in values:
        if value is None:
            continue
        if isinstance(value, str):
            if value.strip():
                return value.strip()
            continue
        return str(value)
    return None


def first_query_value(params: dict[str, list[str]], key: str) -> str | None:
    values = params.get(key)
    if not values:
        return None
    for value in values:
        if value:
            return value
    return None


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_jsonl(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        for record in records:
            handle.write(compact_json(record))
            handle.write("\n")


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def collect_tree_stats(root: Path) -> dict[str, Any]:
    file_count = 0
    total_bytes = 0
    latest_mtime = 0.0
    if root.exists():
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            file_count += 1
            stat = path.stat()
            total_bytes += stat.st_size
            latest_mtime = max(latest_mtime, stat.st_mtime)
    return {
        "file_count": file_count,
        "total_bytes": total_bytes,
        "latest_mtime": int(latest_mtime) if latest_mtime else None,
    }


class DeviceHandler(BaseHTTPRequestHandler):
    server_version = "livekit-esp32s3-device/0.2"
    sys_version = ""

    def _config(self) -> DeviceServiceConfig:
        return self.server.device_service_config  # type: ignore[attr-defined]

    def _request_parts(self) -> tuple[str, dict[str, list[str]]]:
        parsed = urlsplit(self.path)
        return parsed.path, parse_qs(parsed.query, keep_blank_values=True)

    def _request_headers(self) -> dict[str, str]:
        header_names = (
            "Content-Type",
            "Content-Length",
            "User-Agent",
            "X-Device-Id",
            "X-Diag-Kind",
            "X-File-Name",
            "X-Session-Id",
            "X-Boot-Count",
            "X-Reboot-Streak",
            "X-Content-SHA256",
        )
        return {name: self.headers.get(name, "") for name in header_names if self.headers.get(name)}

    def _send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        body = compact_json(payload).encode("utf-8")
        self.send_response(status.value)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self, max_bytes: int) -> bytes:
        content_length = int(self.headers.get("Content-Length", "0") or "0")
        if content_length < 0:
            raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_content_length")
        if content_length > max_bytes:
            raise RequestError(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                "payload_too_large",
                f"max_bytes={max_bytes}",
            )
        return self.rfile.read(content_length) if content_length > 0 else b""

    def _read_json_body(self, max_bytes: int) -> Any:
        raw_body = self._read_body(max_bytes)
        if not raw_body:
            return {}
        try:
            return json.loads(raw_body.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_json", str(exc)) from exc

    def _resolve_device_id(
        self,
        payload: dict[str, Any] | None,
        params: dict[str, list[str]],
        context: dict[str, Any] | None = None,
    ) -> str:
        device_id = first_non_empty(
            payload.get("device_id") if payload else None,
            payload.get("participant_identity") if payload else None,
            payload.get("participant_name") if payload else None,
            context.get("device_id") if context else None,
            context.get("participant_identity") if context else None,
            context.get("participant_name") if context else None,
            first_query_value(params, "device_id"),
            first_query_value(params, "participant_identity"),
            self.headers.get("X-Device-Id"),
        )
        if device_id is None:
            raise RequestError(HTTPStatus.BAD_REQUEST, "missing_device_id")
        return sanitize_segment(device_id, "unknown-device")

    def _handle_health(self) -> None:
        config = self._config()
        self._send_json(
            HTTPStatus.OK,
            {
                "ok": True,
                "service": "device-server",
                "paths": {
                    "legacy_token": config.legacy_token_path,
                    "auth": config.auth_path,
                    "events": config.event_path,
                    "blobs": config.blob_path,
                    "admin": config.admin_path,
                },
                "data_dir": str(config.data_dir),
                "livekit_configured": bool(
                    os.environ.get("LIVEKIT_URL")
                    and os.environ.get("LIVEKIT_API_KEY")
                    and os.environ.get("LIVEKIT_API_SECRET")
                ),
            },
        )

    def _handle_admin_summary(self) -> None:
        config = self._config()
        events_root = config.data_dir / "events"
        blobs_root = config.data_dir / "blobs"
        self._send_json(
            HTTPStatus.OK,
            {
                "ok": True,
                "service": "device-server",
                "data_dir": str(config.data_dir),
                "events": collect_tree_stats(events_root),
                "blobs": collect_tree_stats(blobs_root),
            },
        )

    def _handle_token(self) -> None:
        request = self._read_json_body(max_bytes=16 * 1024)
        if not isinstance(request, dict):
            raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_json_shape", "expected object")

        try:
            response = build_access_token(request, self._config().ttl_seconds)
        except RuntimeError as exc:
            raise RequestError(HTTPStatus.INTERNAL_SERVER_ERROR, "token_build_failed", str(exc)) from exc

        print(
            "[device] auth issued"
            f" room={response['room_name']}"
            f" participant={response['participant_identity']}"
            f" expires_at={response['expires_at']}"
        )
        self._send_json(HTTPStatus.OK, response)

    def _handle_event_ingest(self) -> None:
        config = self._config()
        payload = self._read_json_body(config.max_event_bytes)
        context: dict[str, Any] = {}

        if isinstance(payload, dict) and isinstance(payload.get("events"), list):
            context = {key: value for key, value in payload.items() if key != "events"}
            event_items = payload["events"]
        elif isinstance(payload, list):
            event_items = payload
        elif isinstance(payload, dict):
            event_items = [payload]
        else:
            raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_json_shape", "expected object or array")

        if not event_items:
            raise RequestError(HTTPStatus.BAD_REQUEST, "empty_event_batch")

        request_path, params = self._request_parts()
        del request_path
        batch_id = uuid.uuid4().hex
        received_at = int(time.time())
        touched_paths: set[str] = set()
        accepted = 0

        for index, item in enumerate(event_items):
            if not isinstance(item, dict):
                raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_event", f"index={index}")
            device_id = self._resolve_device_id(item, params, context)
            record = {
                "ingest_id": f"{batch_id}:{index}",
                "received_at": received_at,
                "source_ip": self.client_address[0],
                "headers": self._request_headers(),
                "context": context,
                "event": item,
            }
            event_path = config.data_dir / "events" / time.strftime("%Y%m%d") / f"{device_id}.jsonl"
            write_jsonl(event_path, [record])
            touched_paths.add(str(event_path.relative_to(config.data_dir)))
            accepted += 1

        print(f"[device] diagnostics events accepted={accepted} batch_id={batch_id}")
        self._send_json(
            HTTPStatus.ACCEPTED,
            {
                "ok": True,
                "batch_id": batch_id,
                "accepted": accepted,
                "paths": sorted(touched_paths),
            },
        )

    def _handle_blob_ingest(self) -> None:
        config = self._config()
        path, params = self._request_parts()
        del path
        raw_body = self._read_body(config.max_blob_bytes)
        content_type = (self.headers.get("Content-Type") or "application/octet-stream").split(";", 1)[0].strip()

        payload: dict[str, Any] = {}
        blob_bytes = raw_body
        if content_type == "application/json":
            try:
                payload = json.loads(raw_body.decode("utf-8")) if raw_body else {}
            except json.JSONDecodeError as exc:
                raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_json", str(exc)) from exc
            if not isinstance(payload, dict):
                raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_json_shape", "expected object")
            content_b64 = payload.get("content_b64")
            if not isinstance(content_b64, str) or not content_b64:
                raise RequestError(HTTPStatus.BAD_REQUEST, "missing_content_b64")
            try:
                blob_bytes = base64.b64decode(content_b64, validate=True)
            except ValueError as exc:
                raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_content_b64", str(exc)) from exc
            if len(blob_bytes) > config.max_blob_bytes:
                raise RequestError(
                    HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                    "payload_too_large",
                    f"max_bytes={config.max_blob_bytes}",
                )

        device_id = self._resolve_device_id(payload, params)
        kind = sanitize_segment(
            first_non_empty(
                payload.get("kind"),
                first_query_value(params, "kind"),
                self.headers.get("X-Diag-Kind"),
            ),
            "blob",
        )
        filename = sanitize_segment(
            first_non_empty(
                payload.get("filename"),
                first_query_value(params, "filename"),
                self.headers.get("X-File-Name"),
            ),
            f"{kind}.bin",
        )
        session_id = first_non_empty(
            payload.get("session_id"),
            first_query_value(params, "session_id"),
            self.headers.get("X-Session-Id"),
        )
        expected_sha256 = first_non_empty(
            payload.get("sha256"),
            first_query_value(params, "sha256"),
            self.headers.get("X-Content-SHA256"),
        )
        actual_sha256 = sha256_hex(blob_bytes)
        if expected_sha256 and expected_sha256.lower() != actual_sha256:
            raise RequestError(HTTPStatus.BAD_REQUEST, "sha256_mismatch")

        suffix = Path(filename).suffix or ".bin"
        ingest_id = uuid.uuid4().hex
        day = time.strftime("%Y%m%d")
        blob_dir = config.data_dir / "blobs" / day / device_id
        blob_dir.mkdir(parents=True, exist_ok=True)
        safe_stem = sanitize_segment(Path(filename).stem, kind)
        blob_path = blob_dir / f"{int(time.time() * 1000)}_{kind}_{safe_stem}_{ingest_id}{suffix}"
        meta_path = blob_path.with_suffix(blob_path.suffix + ".json")
        blob_path.write_bytes(blob_bytes)

        metadata = {
            "ingest_id": ingest_id,
            "received_at": int(time.time()),
            "source_ip": self.client_address[0],
            "headers": self._request_headers(),
            "device_id": device_id,
            "kind": kind,
            "filename": filename,
            "session_id": session_id,
            "boot_count": first_non_empty(payload.get("boot_count"), first_query_value(params, "boot_count"), self.headers.get("X-Boot-Count")),
            "reboot_streak": first_non_empty(payload.get("reboot_streak"), first_query_value(params, "reboot_streak"), self.headers.get("X-Reboot-Streak")),
            "content_type": content_type,
            "byte_size": len(blob_bytes),
            "sha256": actual_sha256,
            "metadata": payload.get("metadata") if isinstance(payload.get("metadata"), dict) else None,
        }
        write_json(meta_path, metadata)

        print(
            "[device] diagnostics blob stored"
            f" ingest_id={ingest_id}"
            f" device_id={device_id}"
            f" kind={kind}"
            f" bytes={len(blob_bytes)}"
        )
        self._send_json(
            HTTPStatus.ACCEPTED,
            {
                "ok": True,
                "ingest_id": ingest_id,
                "device_id": device_id,
                "kind": kind,
                "byte_size": len(blob_bytes),
                "sha256": actual_sha256,
                "blob_path": str(blob_path.relative_to(config.data_dir)),
                "meta_path": str(meta_path.relative_to(config.data_dir)),
            },
        )

    def do_GET(self) -> None:  # noqa: N802
        path, _params = self._request_parts()
        try:
            if path == "/healthz":
                self._handle_health()
                return
            if path == self._config().admin_path:
                self._handle_admin_summary()
                return
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
        except RequestError as exc:
            self._send_json(exc.status, {"error": exc.error, **({"detail": exc.detail} if exc.detail else {})})

    def do_POST(self) -> None:  # noqa: N802
        path, _params = self._request_parts()
        try:
            config = self._config()
            if path == config.legacy_token_path or path == config.auth_path:
                self._handle_token()
                return
            if path == config.event_path:
                self._handle_event_ingest()
                return
            if path == config.blob_path:
                self._handle_blob_ingest()
                return
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
        except RequestError as exc:
            self._send_json(exc.status, {"error": exc.error, **({"detail": exc.detail} if exc.detail else {})})
        except Exception as exc:  # noqa: BLE001
            self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": "internal_error", "detail": str(exc)})

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A003
        print(f"[device] {self.client_address[0]} - {format % args}")


def build_service_config(args: argparse.Namespace) -> tuple[str, int, DeviceServiceConfig]:
    host = env_or_default_aliases(args.host, ("DEVICE_SERVER_HOST", "TOKEN_SERVER_HOST"), "0.0.0.0")
    port = env_or_default_int_aliases(args.port, ("DEVICE_SERVER_PORT", "TOKEN_SERVER_PORT"), 8790)
    ttl_seconds = env_or_default_int_aliases(
        args.ttl_seconds,
        ("DEVICE_SERVER_TTL_SECONDS", "TOKEN_SERVER_TTL_SECONDS"),
        3600,
    )
    data_dir = Path(env_or_default(args.data_dir, "DEVICE_SERVER_DATA_DIR", DEFAULT_DATA_DIR)).resolve()
    max_event_bytes = env_or_default_int(
        args.max_event_bytes,
        "DEVICE_SERVER_MAX_EVENT_BYTES",
        DEFAULT_MAX_EVENT_BYTES,
    )
    max_blob_bytes = env_or_default_int(
        args.max_blob_bytes,
        "DEVICE_SERVER_MAX_BLOB_BYTES",
        DEFAULT_MAX_BLOB_BYTES,
    )
    config = DeviceServiceConfig(
        legacy_token_path=DEFAULT_LEGACY_TOKEN_PATH,
        auth_path=DEFAULT_AUTH_PATH,
        event_path=DEFAULT_EVENT_PATH,
        blob_path=DEFAULT_BLOB_PATH,
        admin_path=DEFAULT_ADMIN_PATH,
        ttl_seconds=ttl_seconds,
        data_dir=data_dir,
        max_event_bytes=max_event_bytes,
        max_blob_bytes=max_blob_bytes,
    )
    return host, port, config


def main() -> int:
    args = parse_args()
    load_env_file(args.env_file)
    host, port, config = build_service_config(args)
    config.data_dir.mkdir(parents=True, exist_ok=True)

    server = ThreadingHTTPServer((host, port), DeviceHandler)
    server.device_service_config = config  # type: ignore[attr-defined]

    print(f"[device] listening on http://{host}:{port}")
    print(f"[device] env file: {Path(args.env_file).resolve()}")
    print(f"[device] health:       http://{host}:{port}/healthz")
    print(f"[device] token legacy: http://{host}:{port}{config.legacy_token_path}")
    print(f"[device] token auth:   http://{host}:{port}{config.auth_path}")
    print(f"[device] diag events:  http://{host}:{port}{config.event_path}")
    print(f"[device] diag blobs:   http://{host}:{port}{config.blob_path}")
    print(f"[device] admin:        http://{host}:{port}{config.admin_path}")
    print(f"[device] data dir:     {config.data_dir}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
