#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import json
import signal
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any


def write_wav_header(fp, pcm_bytes: int, sample_rate: int, channels: int, bits_per_sample: int) -> None:
    byte_rate = sample_rate * channels * (bits_per_sample // 8)
    block_align = channels * (bits_per_sample // 8)
    fp.seek(0)
    fp.write(b"RIFF")
    fp.write((pcm_bytes + 36).to_bytes(4, "little"))
    fp.write(b"WAVEfmt ")
    fp.write((16).to_bytes(4, "little"))
    fp.write((1).to_bytes(2, "little"))
    fp.write(channels.to_bytes(2, "little"))
    fp.write(sample_rate.to_bytes(4, "little"))
    fp.write(byte_rate.to_bytes(4, "little"))
    fp.write(block_align.to_bytes(2, "little"))
    fp.write(bits_per_sample.to_bytes(2, "little"))
    fp.write(b"data")
    fp.write(pcm_bytes.to_bytes(4, "little"))
    fp.flush()


@dataclass
class StreamRecorder:
    session_id: str
    stream_name: str = "audio"
    sample_rate: int = 16000
    channels: int = 1
    bits_per_sample: int = 16
    pcm_bytes: int = 0
    frames: int = 0
    file_path: Path | None = None
    file_handle: Any = None
    active_connections: int = 0
    io_lock: asyncio.Lock = field(default_factory=asyncio.Lock)

    async def apply_hello(self, payload: dict[str, Any]) -> None:
        async with self.io_lock:
            self.sample_rate = int(payload.get("sample_rate", self.sample_rate))
            self.channels = int(payload.get("channels", self.channels))
            self.bits_per_sample = int(payload.get("bits_per_sample", self.bits_per_sample))
            write_wav_header(
                self.file_handle,
                self.pcm_bytes,
                self.sample_rate,
                self.channels,
                self.bits_per_sample,
            )

    async def append_pcm(self, message: bytes) -> None:
        async with self.io_lock:
            self.file_handle.seek(0, 2)
            self.file_handle.write(message)
            self.pcm_bytes += len(message)
            self.frames += 1
            if self.frames % 50 == 0:
                self.file_handle.flush()

    async def refresh_header(self) -> None:
        async with self.io_lock:
            write_wav_header(
                self.file_handle,
                self.pcm_bytes,
                self.sample_rate,
                self.channels,
                self.bits_per_sample,
            )
            self.file_handle.flush()

    async def close(self) -> None:
        async with self.io_lock:
            if self.file_handle is None:
                return
            write_wav_header(
                self.file_handle,
                self.pcm_bytes,
                self.sample_rate,
                self.channels,
                self.bits_per_sample,
            )
            self.file_handle.close()
            self.file_handle = None


class SessionManager:
    def __init__(self, output_dir: Path, idle_seconds: float) -> None:
        self.output_dir = output_dir
        self.idle_seconds = idle_seconds
        self.lock = asyncio.Lock()
        self.session_id: str | None = None
        self.last_activity_monotonic = 0.0
        self.recorders: dict[str, StreamRecorder] = {}

    async def _roll_session_locked(self) -> None:
        old_recorders = list(self.recorders.values())
        self.recorders = {}
        self.session_id = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.last_activity_monotonic = time.monotonic()
        for recorder in old_recorders:
            await recorder.close()
        print(f"[ws] session start: {self.session_id}")

    def _has_active_connections_locked(self) -> bool:
        return any(recorder.active_connections > 0 for recorder in self.recorders.values())

    async def acquire(self, stream_name: str) -> StreamRecorder:
        async with self.lock:
            now = time.monotonic()
            session_expired = (
                self.session_id is None or (
                    not self._has_active_connections_locked() and
                    (now - self.last_activity_monotonic) >= self.idle_seconds
                )
            )
            if session_expired:
                await self._roll_session_locked()

            assert self.session_id is not None
            recorder = self.recorders.get(stream_name)
            if recorder is None:
                file_path, file_handle = open_session_file(self.output_dir, stream_name, self.session_id)
                recorder = StreamRecorder(
                    session_id=self.session_id,
                    stream_name=stream_name,
                    file_path=file_path,
                    file_handle=file_handle,
                )
                self.recorders[stream_name] = recorder
                print(f"[ws] open stream={stream_name} session={self.session_id} -> {file_path}")

            recorder.active_connections += 1
            self.last_activity_monotonic = now
            return recorder

    async def touch(self) -> None:
        async with self.lock:
            self.last_activity_monotonic = time.monotonic()

    async def release(self, recorder: StreamRecorder) -> None:
        async with self.lock:
            recorder.active_connections = max(0, recorder.active_connections - 1)
            self.last_activity_monotonic = time.monotonic()
            should_refresh = recorder.active_connections == 0
        if should_refresh:
            await recorder.refresh_header()
            print(
                f"[ws] idle stream={recorder.stream_name} session={recorder.session_id} "
                f"frames={recorder.frames} bytes={recorder.pcm_bytes} file={recorder.file_path}"
            )

    async def close_all(self) -> None:
        async with self.lock:
            recorders = list(self.recorders.values())
            self.recorders = {}
        for recorder in recorders:
            await recorder.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Receive ESP32 debug PCM over WebSocket and write WAV files")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--path", action="append", dest="paths", default=[])
    parser.add_argument("--output-dir", default="./debug_uplink_ws")
    parser.add_argument("--session-idle-seconds", type=float, default=20.0)
    return parser.parse_args()


def open_session_file(output_dir: Path, stream_name: str, session_id: str) -> tuple[Path, Any]:
    safe_stream = "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in stream_name) or "audio"
    path = output_dir / f"{safe_stream}_{session_id}.wav"
    fp = path.open("wb+")
    write_wav_header(fp, 0, 16000, 1, 16)
    return path, fp


async def handler(websocket, manager: SessionManager, expected_paths: set[str]) -> None:
    request_path = getattr(websocket, "path", None)
    if request_path is None:
        request = getattr(websocket, "request", None)
        request_path = getattr(request, "path", None)
    if request_path and request_path not in expected_paths:
        await websocket.close(code=1008, reason=f"expected one of {sorted(expected_paths)}")
        return

    stream_name = (request_path or "/audio").strip("/") or "audio"
    recorder = await manager.acquire(stream_name)
    print(
        f"[ws] connected path={request_path or '/audio'} stream={recorder.stream_name} "
        f"session={recorder.session_id} -> {recorder.file_path}"
    )

    try:
        async for message in websocket:
            if isinstance(message, str):
                try:
                    payload = json.loads(message)
                except json.JSONDecodeError:
                    print(f"[ws] text={message}")
                    continue
                if payload.get("type") == "hello":
                    await recorder.apply_hello(payload)
                    print(
                        f"[ws] hello stream={recorder.stream_name} session={recorder.session_id} "
                        f"format=pcm_s16le rate={recorder.sample_rate} "
                        f"channels={recorder.channels} bits={recorder.bits_per_sample}"
                    )
                else:
                    print(f"[ws] json={payload}")
                continue

            await recorder.append_pcm(message)
            await manager.touch()
            if recorder.frames % 200 == 0:
                print(
                    f"[ws] stream={recorder.stream_name} session={recorder.session_id} "
                    f"frames={recorder.frames} bytes={recorder.pcm_bytes}"
                )
    except asyncio.CancelledError:
        raise
    except Exception as exc:
        close_code = getattr(websocket, "close_code", None)
        if close_code is not None:
            print(
                f"[ws] disconnected stream={recorder.stream_name} "
                f"session={recorder.session_id} code={close_code} err={exc}"
            )
        else:
            print(f"[ws] disconnected stream={recorder.stream_name} session={recorder.session_id} err={exc}")
    finally:
        await manager.release(recorder)


async def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    expected_paths = set(args.paths or ["/uplink", "/downlink"])
    manager = SessionManager(output_dir, args.session_idle_seconds)

    import websockets

    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            pass

    async with websockets.serve(
        lambda ws: handler(ws, manager, expected_paths),
        args.host,
        args.port,
        max_size=None,
        ping_interval=20,
        ping_timeout=20,
    ):
        print(f"[ws] listening on ws://{args.host}:{args.port} paths={sorted(expected_paths)}")
        print(f"[ws] output dir: {output_dir.resolve()}")
        await stop_event.wait()

    await manager.close_all()
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
