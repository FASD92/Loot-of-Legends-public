#!/usr/bin/env python3
"""Run Release 1 concurrent active-room probes across participant counts."""

from __future__ import annotations

import argparse
import json
import socket
import sys
from pathlib import Path
from typing import Any, Callable, Sequence

from scripts.release1.capacity_sweep import parse_participant_counts
from scripts.release1.concurrent_capacity_probe import run_concurrent_capacity_probe


SCHEMA_VERSION = "release1.concurrent_capacity_sweep.v1"
RunOne = Callable[..., dict[str, Any]]


def run_concurrent_capacity_sweep(
    *,
    run_id_prefix: str,
    artifact_root: Path | str,
    server_bin: Path | str,
    host: str,
    participant_counts: Sequence[int],
    room_size: int,
    timeout_sec: float,
    auth_token_prefix: str | None = None,
    move_duration_sec: float = 0.0,
    move_rate_hz: float = 0.0,
    attack_duration_sec: float = 0.0,
    attack_rate_hz: float = 0.0,
    battle_cycles: int = 1,
    first_client_id: int = 100000,
    client_id_stride: int = 1000,
    rudp_handshake_settle_sec: float = 0.0,
    runner_delivery_min_ratio: float = 0.99,
    primary_latency_min_samples: int = 100,
    primary_latency_p99_slo_ms: float = 50.0,
    ready_timeout_sec: float = 5.0,
    shutdown_timeout_sec: float = 5.0,
    post_runner_metrics_settle_sec: float = 0.0,
    metrics_interval_ms: int = 50,
    max_workers: int | None = None,
    server_cwd: Path | str | None = None,
    allocate_port: Callable[[str], int] | None = None,
    run_one: RunOne = run_concurrent_capacity_probe,
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
                artifact_dir=artifact_dir,
                server_argv=server_argv,
                host=host,
                tcp_port=tcp_port,
                participants=participants,
                room_size=room_size,
                timeout_sec=timeout_sec,
                udp_port=tcp_port,
                auth_token_prefix=auth_token_prefix,
                move_duration_sec=move_duration_sec,
                move_rate_hz=move_rate_hz,
                attack_duration_sec=attack_duration_sec,
                attack_rate_hz=attack_rate_hz,
                battle_cycles=battle_cycles,
                first_client_id=first_client_id,
                client_id_stride=client_id_stride,
                rudp_handshake_settle_sec=rudp_handshake_settle_sec,
                runner_delivery_min_ratio=runner_delivery_min_ratio,
                primary_latency_min_samples=primary_latency_min_samples,
                primary_latency_p99_slo_ms=primary_latency_p99_slo_ms,
                ready_timeout_sec=ready_timeout_sec,
                shutdown_timeout_sec=shutdown_timeout_sec,
                post_runner_metrics_settle_sec=post_runner_metrics_settle_sec,
                max_workers=max_workers,
                server_cwd=server_cwd,
            )
            runs.append(_compact_run(participants, run_id, artifact_dir, result))
        except (OSError, ValueError, json.JSONDecodeError) as error:
            runs.append(
                {
                    "participants": participants,
                    "run_id": run_id,
                    "artifact_dir": str(artifact_dir),
                    "valid": False,
                    "result_status": "INVALID",
                    "reason": str(error),
                    "valid_groups": 0,
                    "group_count": participants // room_size,
                    "runner_delivery_status": "",
                    "server_accepted_delivery_status": "",
                    "primary_latency_status": "",
                }
            )

    summary = {
        "schema_version": SCHEMA_VERSION,
        "run_id_prefix": run_id_prefix,
        "artifact_root": str(root),
        "room_size": room_size,
        "participant_counts": counts,
        "highest_passing_participants": _highest_passing_participants(runs),
        "first_non_passing_participants": _first_non_passing_participants(runs),
        "runs": runs,
    }
    _write_json(root / "concurrent_capacity_sweep.json", summary)
    return summary


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Release 1 concurrent active-room probes across participant counts."
    )
    parser.add_argument("--run-id-prefix", required=True)
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--participant-counts", required=True)
    parser.add_argument("--room-size", default=10, type=int)
    parser.add_argument("--timeout-sec", default=5.0, type=float)
    parser.add_argument("--auth-token-prefix")
    parser.add_argument("--move-duration-sec", default=0.0, type=float)
    parser.add_argument("--move-rate-hz", default=0.0, type=float)
    parser.add_argument("--attack-duration-sec", default=0.0, type=float)
    parser.add_argument("--attack-rate-hz", default=0.0, type=float)
    parser.add_argument("--battle-cycles", default=1, type=int)
    parser.add_argument("--first-client-id", default=100000, type=int)
    parser.add_argument("--client-id-stride", default=1000, type=int)
    parser.add_argument("--rudp-handshake-settle-sec", default=0.0, type=float)
    parser.add_argument("--runner-delivery-min-ratio", default=0.99, type=float)
    parser.add_argument("--primary-latency-min-samples", default=100, type=int)
    parser.add_argument("--primary-latency-p99-slo-ms", default=50.0, type=float)
    parser.add_argument("--ready-timeout-sec", default=5.0, type=float)
    parser.add_argument("--shutdown-timeout-sec", default=5.0, type=float)
    parser.add_argument("--post-runner-metrics-settle-sec", default=0.0, type=float)
    parser.add_argument("--metrics-interval-ms", default=50, type=int)
    parser.add_argument("--max-workers", type=int)
    parser.add_argument("--server-cwd")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = run_concurrent_capacity_sweep(
            run_id_prefix=args.run_id_prefix,
            artifact_root=args.artifact_root,
            server_bin=args.server_bin,
            host=args.host,
            participant_counts=parse_participant_counts(args.participant_counts),
            room_size=args.room_size,
            timeout_sec=args.timeout_sec,
            auth_token_prefix=args.auth_token_prefix,
            move_duration_sec=args.move_duration_sec,
            move_rate_hz=args.move_rate_hz,
            attack_duration_sec=args.attack_duration_sec,
            attack_rate_hz=args.attack_rate_hz,
            battle_cycles=args.battle_cycles,
            first_client_id=args.first_client_id,
            client_id_stride=args.client_id_stride,
            rudp_handshake_settle_sec=args.rudp_handshake_settle_sec,
            runner_delivery_min_ratio=args.runner_delivery_min_ratio,
            primary_latency_min_samples=args.primary_latency_min_samples,
            primary_latency_p99_slo_ms=args.primary_latency_p99_slo_ms,
            ready_timeout_sec=args.ready_timeout_sec,
            shutdown_timeout_sec=args.shutdown_timeout_sec,
            post_runner_metrics_settle_sec=args.post_runner_metrics_settle_sec,
            metrics_interval_ms=args.metrics_interval_ms,
            max_workers=args.max_workers,
            server_cwd=args.server_cwd,
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"concurrent capacity sweep failed: {error}", file=sys.stderr)
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


def _compact_run(
    participants: int,
    run_id: str,
    artifact_dir: Path,
    result: dict[str, Any],
) -> dict[str, Any]:
    return {
        "participants": participants,
        "run_id": run_id,
        "artifact_dir": str(artifact_dir),
        "valid": bool(result.get("valid")),
        "result_status": str(result.get("evaluation", {}).get("result_status", "INVALID")),
        "reason": str(result.get("reason", "")),
        "valid_groups": int(result.get("valid_groups", 0) or 0),
        "group_count": int(result.get("group_count", 0) or 0),
        "runner_delivery_status": _result_status(result, "runner_delivery_evaluation"),
        "server_accepted_delivery_status": _result_status(
            result,
            "server_accepted_delivery_evaluation",
        ),
        "primary_latency_status": _result_status(result, "primary_latency_evaluation"),
    }


def _result_status(result: dict[str, Any], key: str) -> str:
    return str(result.get(key, {}).get("result_status", ""))


def _highest_passing_participants(runs: Sequence[dict[str, Any]]) -> int | None:
    passing = [
        int(run["participants"])
        for run in runs
        if run["valid"] and run["result_status"] == "PASS"
    ]
    return max(passing) if passing else None


def _first_non_passing_participants(runs: Sequence[dict[str, Any]]) -> int | None:
    for run in runs:
        if not run["valid"] or run["result_status"] != "PASS":
            return int(run["participants"])
    return None


def _write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
