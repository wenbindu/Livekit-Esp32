#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Local LiveKit token server for livekit-esp32s3")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8790)
    parser.add_argument("--path", default="/token")
    parser.add_argument("--env-file", default="configs/token_server.local.env")
    parser.add_argument("--ttl-seconds", type=int, default=3600)
    return parser.parse_args()


def load_env_file(path: str) -> None:
    env_path = Path(path)
    if not env_path.exists():
        return

    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        os.environ.setdefault(key, value)


def b64url_json(value: dict[str, Any]) -> str:
    return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode("utf-8")).rstrip(b"=").decode("ascii")


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


class TokenHandler(BaseHTTPRequestHandler):
    server_version = "livekit-esp32s3-token/0.1"
    token_path = "/token"
    ttl_seconds = 3600

    def _send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status.value)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/healthz":
            self._send_json(HTTPStatus.OK, {"ok": True})
            return
        self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})

    def do_POST(self) -> None:  # noqa: N802
        if self.path != self.token_path:
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
            return

        content_length = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(content_length) if content_length > 0 else b"{}"
        try:
            request = json.loads(raw_body.decode("utf-8") or "{}")
        except json.JSONDecodeError:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid_json"})
            return

        try:
            response = build_access_token(request, self.ttl_seconds)
        except Exception as exc:  # noqa: BLE001
            self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})
            return
        print(f"response: {response}")
        self._send_json(HTTPStatus.OK, response)

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A003
        print(f"[token] {self.address_string()} - {format % args}")


def main() -> int:
    args = parse_args()
    load_env_file(args.env_file)

    TokenHandler.token_path = args.path
    TokenHandler.ttl_seconds = args.ttl_seconds

    server = ThreadingHTTPServer((args.host, args.port), TokenHandler)
    print(f"[token] listening on http://{args.host}:{args.port}{args.path}")
    print(f"[token] env file: {Path(args.env_file).resolve()}")
    print(f"[token] health:  http://{args.host}:{args.port}/healthz")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
