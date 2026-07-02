#!/usr/bin/env python3
"""Run Release 1 official-capacity smoke across participant counts."""

from __future__ import annotations

import argparse
import json
import socket
import sys
from pathlib import Path
from typing import Any, Callable, Sequence

from scripts.release1.official_capacity_run import run_official_capacity_smoke


SCHEMA_VERSION = "release1.capacity_sweep.v1"
RunOne = Callable[..., dict[str, Any]]


def parse_participant_counts(raw: str) -> list[int]:
    values = [part.strip() for part in raw.split(",")]
    if not values or any(value == "" for value in values):
        raise ValueError("participant counts must be comma-separated integers")

    counts = []
    for value in values:
        try:
            count = int(value)
        except ValueError as error:
            raise ValueError("participant counts must be integers") from error
        if count <= 0:
            raise ValueError("participant counts must be positive")
        counts.append(count)

    if len(set(counts)) != len(counts):
        raise ValueError("participant counts must not contain duplicates")
    return counts


def run_capacity_sweep(
    *,
    run_id_prefix: str,
    artifact_root: Path | str,
    server_bin: Path | str,
    host: str,
    participant_counts: Sequence[int],
    room_size: int,
    timeout_sec: float,
    auth_token_prefix: str | None = None,
    official_window_sec: float | None = None,
    move_duration_sec: float = 0.0,
    move_rate_hz: float = 0.0,
    attack_duration_sec: float = 0.0,
    attack_rate_hz: float = 0.0,
    battle_cycles: int = 1,
    runner_delivery_min_ratio: float = 0.99,
    ready_timeout_sec: float = 5.0,
    shutdown_timeout_sec: float = 5.0,
    post_runner_metrics_settle_sec: float = 0.0,
    metrics_interval_ms: int = 50,
    server_cwd: Path | str | None = None,
    stress_runner_argv: Sequence[str] | None = None,
    allocate_port: Callable[[str], int] | None = None,
    run_one: RunOne = run_official_capacity_smoke,
) -> dict[str, Any]:
    counts = _validated_counts(participant_counts, room_size)
    root = Path(artifact_root)
    root.mkdir(parents=True, exist_ok=True)

    runs = []
    for participants in counts:
        run_id = f"{run_id_prefix}-n{participants}"
        artifact_dir = root / run_id
        tcp_port = (allocate_port or _free_tcp_port)(host)
        metrics_path = artifact_dir / "lol_game.prom"
        server_argv = [
            str(server_bin),
            str(tcp_port),
            "--metrics-textfile",
            str(metrics_path),
            "--metrics-interval-ms",
            str(metrics_interval_ms),
        ]

        try:
            result = run_one(
                run_id=run_id,
                artifact_dir=artifact_dir,
                server_argv=server_argv,
                host=host,
                tcp_port=tcp_port,
                participants=participants,
                room_size=room_size,
                timeout_sec=timeout_sec,
                udp_port=tcp_port,
                auth_token_prefix=auth_token_prefix,
                official_window_sec=official_window_sec,
                move_duration_sec=move_duration_sec,
                move_rate_hz=move_rate_hz,
                attack_duration_sec=attack_duration_sec,
                attack_rate_hz=attack_rate_hz,
                battle_cycles=battle_cycles,
                runner_delivery_min_ratio=runner_delivery_min_ratio,
                ready_timeout_sec=ready_timeout_sec,
                shutdown_timeout_sec=shutdown_timeout_sec,
                post_runner_metrics_settle_sec=post_runner_metrics_settle_sec,
                server_cwd=server_cwd,
                stress_runner_argv=stress_runner_argv,
            )
            runs.append(_compact_run(participants, result))
        except (OSError, ValueError, json.JSONDecodeError) as error:
            runs.append(
                {
                    "participants": participants,
                    "run_id": run_id,
                    "artifact_dir": str(artifact_dir),
                    "valid": False,
                    "result_status": "INVALID",
                    "reason": str(error),
                    "official_window_completed": False,
                }
            )

    highest = _highest_passing_participants(runs)
    summary = {
        "schema_version": SCHEMA_VERSION,
        "run_id_prefix": run_id_prefix,
        "artifact_root": str(root),
        "room_size": room_size,
        "participant_counts": counts,
        "highest_passing_participants": highest,
        "highest_passing_artifact_dir": _artifact_dir_for_participants(runs, highest),
        "runs": runs,
    }
    _write_json(root / "capacity_sweep.json", summary)
    return summary


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Release 1 official-capacity smoke across participant counts."
    )
    parser.add_argument("--run-id-prefix", required=True)
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--participant-counts", required=True)
    parser.add_argument("--room-size", default=2, type=int)
    parser.add_argument("--timeout-sec", default=5.0, type=float)
    parser.add_argument("--auth-token-prefix")
    parser.add_argument("--official-window-sec", type=float)
    parser.add_argument("--move-duration-sec", default=0.0, type=float)
    parser.add_argument("--move-rate-hz", default=0.0, type=float)
    parser.add_argument("--attack-duration-sec", default=0.0, type=float)
    parser.add_argument("--attack-rate-hz", default=0.0, type=float)
    parser.add_argument("--battle-cycles", default=1, type=int)
    parser.add_argument("--runner-delivery-min-ratio", default=0.99, type=float)
    parser.add_argument("--ready-timeout-sec", default=5.0, type=float)
    parser.add_argument("--shutdown-timeout-sec", default=5.0, type=float)
    parser.add_argument("--post-runner-metrics-settle-sec", default=0.0, type=float)
    parser.add_argument("--metrics-interval-ms", default=50, type=int)
    parser.add_argument("--server-cwd")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = run_capacity_sweep(
            run_id_prefix=args.run_id_prefix,
            artifact_root=args.artifact_root,
            server_bin=args.server_bin,
            host=args.host,
            participant_counts=parse_participant_counts(args.participant_counts),
            room_size=args.room_size,
            timeout_sec=args.timeout_sec,
            auth_token_prefix=args.auth_token_prefix,
            official_window_sec=args.official_window_sec,
            move_duration_sec=args.move_duration_sec,
            move_rate_hz=args.move_rate_hz,
            attack_duration_sec=args.attack_duration_sec,
            attack_rate_hz=args.attack_rate_hz,
            battle_cycles=args.battle_cycles,
            runner_delivery_min_ratio=args.runner_delivery_min_ratio,
            ready_timeout_sec=args.ready_timeout_sec,
            shutdown_timeout_sec=args.shutdown_timeout_sec,
            post_runner_metrics_settle_sec=args.post_runner_metrics_settle_sec,
            metrics_interval_ms=args.metrics_interval_ms,
            server_cwd=args.server_cwd,
            stress_runner_argv=[sys.executable, "-m", "scripts.release1.capacity_sweep"]
            + (argv or sys.argv[1:]),
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"capacity sweep failed: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, sort_keys=True))
    return 0 if result["highest_passing_participants"] is not None else 1


def _validated_counts(participant_counts: Sequence[int], room_size: int) -> list[int]:
    if room_size <= 0:
        raise ValueError("room_size must be positive")
    counts = list(participant_counts)
    if not counts:
        raise ValueError("participant counts must not be empty")
    for count in counts:
        if count <= 0:
            raise ValueError("participant counts must be positive")
        if count % room_size != 0:
            raise ValueError("participant counts must be divisible by room_size")
    if len(set(counts)) != len(counts):
        raise ValueError("participant counts must not contain duplicates")
    return counts


def _free_tcp_port(host: str) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def _compact_run(participants: int, result: dict[str, Any]) -> dict[str, Any]:
    return {
        "participants": participants,
        "run_id": str(result.get("run_id", "")),
        "artifact_dir": str(result.get("artifact_dir", "")),
        "valid": bool(result.get("valid")),
        "result_status": str(result.get("evaluation", {}).get("result_status", "INVALID")),
        "reason": str(result.get("reason", "")),
        "official_window_completed": bool(
            result.get("official_window", {}).get("completed")
        ),
    }


def _highest_passing_participants(runs: Sequence[dict[str, Any]]) -> int | None:
    passing = [
        int(run["participants"])
        for run in runs
        if run["valid"] and run["result_status"] == "PASS"
    ]
    return max(passing) if passing else None


def _artifact_dir_for_participants(
    runs: Sequence[dict[str, Any]],
    participants: int | None,
) -> str | None:
    if participants is None:
        return None
    for run in runs:
        if run["participants"] == participants:
            return str(run["artifact_dir"])
    return None


def _write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
