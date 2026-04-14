#!/usr/bin/env python3
from __future__ import annotations

import runpy
import sys
from pathlib import Path


def has_flag(flag: str) -> bool:
    return any(arg == flag or arg.startswith(f"{flag}=") for arg in sys.argv[1:])


def main() -> int:
    repo_dir = Path(__file__).resolve().parents[1]
    target = repo_dir / "device_server" / "scripts" / "device_server.py"

    if not has_flag("--env-file"):
        sys.argv.extend(["--env-file", str(repo_dir / "configs" / "token_server.local.env")])
    if not has_flag("--data-dir"):
        sys.argv.extend(["--data-dir", str(repo_dir / ".run" / "device_server")])

    runpy.run_path(str(target), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
