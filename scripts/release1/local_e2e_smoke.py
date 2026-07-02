#!/usr/bin/env python3
"""Run one local Release 1 server + external runner smoke."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Sequence

from scripts.release1.external_stress_runner import run_external_tcp_room_fill
from scripts.release1.server_process import _metrics_ready, _stop_process


def run_local_e2e_smoke(
    server_argv: Sequence[str],
    metrics_textfile_path: Path | str,
    server_log_path: Path | str,
    runner_output_path: Path | str,
    host: str,
    tcp_port: int,
    participants: int,
    room_size: int,
    timeout_sec: float,
    udp_port: int | None = None,
    auth_token_prefix: str | None = None,
    start_battle: bool = False,
    loot_smoke: bool = False,
    move_duration_sec: float = 0.0,
    move_rate_hz: float = 0.0,
    attack_duration_sec: float = 0.0,
    attack_rate_hz: float = 0.0,
    battle_cycles: int = 1,
    ready_timeout_sec: float = 5.0,
    shutdown_timeout_sec: float = 5.0,
    poll_interval_sec: float = 0.05,
    post_runner_metrics_settle_sec: float = 0.0,
    server_cwd: Path | str | None = None,
) -> dict[str, Any]:
    if not server_argv:
        raise ValueError("server argv is required")

    metrics_path = Path(metrics_textfile_path)
    log_path = Path(server_log_path)
    output_path = Path(runner_output_path)
    metrics_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if metrics_path.exists():
        metrics_path.unlink()

    result = _base_result(metrics_path, log_path, output_path)
    with log_path.open("wb") as log_file:
        process = subprocess.Popen(  # noqa: S603 - direct argv, no shell.
            list(server_argv),
            cwd=str(server_cwd) if server_cwd is not None else None,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
        result["server"]["started"] = True
        try:
            _wait_until_ready(process, metrics_path, result, ready_timeout_sec, poll_interval_sec)
            if result["server"]["ready"]:
                runner = run_external_tcp_room_fill(
                    host=host,
                    tcp_port=tcp_port,
                    participants=participants,
                    room_size=room_size,
                    timeout_sec=timeout_sec,
                    udp_port=udp_port,
                    auth_token_prefix=auth_token_prefix,
                    start_battle=start_battle,
                    loot_smoke=loot_smoke,
                    move_duration_sec=move_duration_sec,
                    move_rate_hz=move_rate_hz,
                    attack_duration_sec=attack_duration_sec,
                    attack_rate_hz=attack_rate_hz,
                    battle_cycles=battle_cycles,
                )
                result["runner"] = runner
                _write_json(output_path, runner)
                if post_runner_metrics_settle_sec > 0:
                    time.sleep(post_runner_metrics_settle_sec)
        finally:
            _stop_process(process, result["server"], shutdown_timeout_sec)

    result["valid"] = (
        result["server"]["ready"] and
        result["server"]["stopped"] and
        not result["server"]["killed"] and
        bool(result["runner"].get("valid"))
    )
    if result["valid"]:
        result["reason"] = ""
    elif result["server"]["reason"]:
        result["reason"] = result["server"]["reason"]
    else:
        result["reason"] = str(result["runner"].get("reason", "runner_failed"))
    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run one local Release 1 server + external runner smoke."
    )
    parser.add_argument("--metrics-textfile", required=True)
    parser.add_argument("--server-log", required=True)
    parser.add_argument("--runner-output", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--tcp-port", required=True, type=int)
    parser.add_argument("--participants", required=True, type=int)
    parser.add_argument("--room-size", required=True, type=int)
    parser.add_argument("--timeout-sec", default=5.0, type=float)
    parser.add_argument("--udp-port", type=int)
    parser.add_argument("--auth-token-prefix")
    parser.add_argument("--start-battle", action="store_true")
    parser.add_argument("--loot-smoke", action="store_true")
    parser.add_argument("--move-duration-sec", default=0.0, type=float)
    parser.add_argument("--move-rate-hz", default=0.0, type=float)
    parser.add_argument("--attack-duration-sec", default=0.0, type=float)
    parser.add_argument("--attack-rate-hz", default=0.0, type=float)
    parser.add_argument("--battle-cycles", default=1, type=int)
    parser.add_argument("--ready-timeout-sec", default=5.0, type=float)
    parser.add_argument("--shutdown-timeout-sec", default=5.0, type=float)
    parser.add_argument("--poll-interval-sec", default=0.05, type=float)
    parser.add_argument("--post-runner-metrics-settle-sec", default=0.0, type=float)
    parser.add_argument("--server-cwd")
    parser.add_argument("server_argv", nargs=argparse.REMAINDER)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = run_local_e2e_smoke(
            server_argv=_server_argv(args.server_argv),
            metrics_textfile_path=args.metrics_textfile,
            server_log_path=args.server_log,
            runner_output_path=args.runner_output,
            host=args.host,
            tcp_port=args.tcp_port,
            participants=args.participants,
            room_size=args.room_size,
            timeout_sec=args.timeout_sec,
            udp_port=args.udp_port,
            auth_token_prefix=args.auth_token_prefix,
            start_battle=args.start_battle,
            loot_smoke=args.loot_smoke,
            move_duration_sec=args.move_duration_sec,
            move_rate_hz=args.move_rate_hz,
            attack_duration_sec=args.attack_duration_sec,
            attack_rate_hz=args.attack_rate_hz,
            battle_cycles=args.battle_cycles,
            ready_timeout_sec=args.ready_timeout_sec,
            shutdown_timeout_sec=args.shutdown_timeout_sec,
            poll_interval_sec=args.poll_interval_sec,
            post_runner_metrics_settle_sec=args.post_runner_metrics_settle_sec,
            server_cwd=args.server_cwd,
        )
    except (OSError, ValueError) as error:
        print(f"local e2e smoke failed: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, sort_keys=True))
    return 0 if result["valid"] else 1


def _base_result(
    metrics_path: Path,
    log_path: Path,
    output_path: Path,
) -> dict[str, Any]:
    return {
        "schema_version": "release1.local_e2e_smoke.v1",
        "valid": False,
        "reason": "",
        "server": {
            "started": False,
            "ready": False,
            "stopped": False,
            "terminated": False,
            "killed": False,
            "return_code": None,
            "reason": "",
            "metrics_textfile_path": str(metrics_path),
            "log_path": str(log_path),
        },
        "runner": {},
        "runner_output_path": str(output_path),
    }


def _wait_until_ready(
    process: subprocess.Popen[bytes],
    metrics_path: Path,
    result: dict[str, Any],
    ready_timeout_sec: float,
    poll_interval_sec: float,
) -> None:
    deadline = time.monotonic() + ready_timeout_sec
    while time.monotonic() < deadline:
        if _metrics_ready(metrics_path):
            result["server"]["ready"] = True
            return

        return_code = process.poll()
        if return_code is not None:
            result["server"]["return_code"] = return_code
            result["server"]["reason"] = "process_exited_before_ready"
            return

        time.sleep(poll_interval_sec)

    result["server"]["reason"] = "metrics_ready_timeout"


def _server_argv(values: Sequence[str]) -> list[str]:
    argv = list(values)
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        raise ValueError("server argv is required after --")
    return argv


def _write_json(path: Path, data: dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
