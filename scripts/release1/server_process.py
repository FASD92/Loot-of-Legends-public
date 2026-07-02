#!/usr/bin/env python3
"""Launch one Release 1 server process and verify metrics readiness."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Sequence


READY_METRIC = "lol_server_runtime_tick_total"


def run_server_process_lifecycle(
    argv: Sequence[str],
    metrics_textfile_path: Path | str,
    log_path: Path | str,
    ready_timeout_sec: float = 5.0,
    shutdown_timeout_sec: float = 5.0,
    poll_interval_sec: float = 0.05,
    cwd: Path | str | None = None,
) -> dict[str, Any]:
    if not argv:
        raise ValueError("server argv is required")

    metrics_path = Path(metrics_textfile_path)
    output_log_path = Path(log_path)
    metrics_path.parent.mkdir(parents=True, exist_ok=True)
    output_log_path.parent.mkdir(parents=True, exist_ok=True)
    if metrics_path.exists():
        metrics_path.unlink()

    result: dict[str, Any] = {
        "started": False,
        "ready": False,
        "stopped": False,
        "terminated": False,
        "killed": False,
        "return_code": None,
        "reason": "",
        "metrics_textfile_path": str(metrics_path),
        "log_path": str(output_log_path),
    }

    with output_log_path.open("wb") as log_file:
        process = subprocess.Popen(  # noqa: S603 - direct argv, no shell.
            list(argv),
            cwd=str(cwd) if cwd is not None else None,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
        result["started"] = True
        try:
            _wait_for_readiness(
                process,
                metrics_path,
                result,
                ready_timeout_sec,
                poll_interval_sec,
            )
        finally:
            _stop_process(process, result, shutdown_timeout_sec)

    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Launch one Release 1 server process and wait for readiness."
    )
    parser.add_argument("--metrics-textfile", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--ready-timeout-sec", type=float, default=5.0)
    parser.add_argument("--shutdown-timeout-sec", type=float, default=5.0)
    parser.add_argument("--poll-interval-sec", type=float, default=0.05)
    parser.add_argument("--cwd")
    parser.add_argument("server_argv", nargs=argparse.REMAINDER)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = run_server_process_lifecycle(
            argv=_server_argv(args.server_argv),
            metrics_textfile_path=args.metrics_textfile,
            log_path=args.log,
            ready_timeout_sec=args.ready_timeout_sec,
            shutdown_timeout_sec=args.shutdown_timeout_sec,
            poll_interval_sec=args.poll_interval_sec,
            cwd=args.cwd,
        )
    except (OSError, ValueError) as error:
        print(f"server process lifecycle failed: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, sort_keys=True))
    return 0 if result["ready"] and result["stopped"] and not result["killed"] else 1


def _wait_for_readiness(
    process: subprocess.Popen[bytes],
    metrics_path: Path,
    result: dict[str, Any],
    ready_timeout_sec: float,
    poll_interval_sec: float,
) -> None:
    deadline = time.monotonic() + ready_timeout_sec
    while time.monotonic() < deadline:
        if _metrics_ready(metrics_path):
            result["ready"] = True
            result["reason"] = ""
            return

        return_code = process.poll()
        if return_code is not None:
            result["return_code"] = return_code
            result["reason"] = "process_exited_before_ready"
            return

        time.sleep(poll_interval_sec)

    result["reason"] = "metrics_ready_timeout"


def _metrics_ready(metrics_path: Path) -> bool:
    try:
        return READY_METRIC in metrics_path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return False


def _stop_process(
    process: subprocess.Popen[bytes],
    result: dict[str, Any],
    shutdown_timeout_sec: float,
) -> None:
    return_code = process.poll()
    if return_code is not None:
        result["return_code"] = return_code
        result["stopped"] = True
        return

    process.terminate()
    result["terminated"] = True
    try:
        result["return_code"] = process.wait(timeout=shutdown_timeout_sec)
    except subprocess.TimeoutExpired:
        process.kill()
        result["killed"] = True
        result["return_code"] = process.wait()
    result["stopped"] = True


def _server_argv(values: Sequence[str]) -> list[str]:
    argv = list(values)
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        raise ValueError("server argv is required after --")
    return argv


if __name__ == "__main__":
    raise SystemExit(main())
