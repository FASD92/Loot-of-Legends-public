#!/usr/bin/env python3
"""Run the Release 1 official-capacity smoke artifact pipeline."""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any, Sequence

from scripts.release1.artifact_assembly import assemble_artifact_bundle
from scripts.release1.capacity_report import (
    evaluate_runner_delivery_from_path,
    evaluate_server_accepted_delivery_from_paths,
    load_json,
)
from scripts.release1.local_e2e_smoke import run_local_e2e_smoke
from scripts.release1.run_config import build_execution_target, build_run_config
from scripts.release1.summary_orchestrator import build_summary


def run_official_capacity_smoke(
    *,
    run_id: str,
    artifact_dir: Path | str,
    server_argv: Sequence[str],
    host: str,
    tcp_port: int,
    participants: int,
    room_size: int,
    timeout_sec: float,
    udp_port: int | None = None,
    auth_token_prefix: str | None = None,
    completed_official_window: bool = False,
    official_window_sec: float | None = None,
    move_duration_sec: float = 0.0,
    move_rate_hz: float = 0.0,
    attack_duration_sec: float = 0.0,
    attack_rate_hz: float = 0.0,
    battle_cycles: int = 1,
    runner_delivery_min_ratio: float = 0.99,
    gate_config_path: Path | str = Path(__file__).with_name("gate_config.json"),
    ready_timeout_sec: float = 5.0,
    shutdown_timeout_sec: float = 5.0,
    post_runner_metrics_settle_sec: float = 0.0,
    server_cwd: Path | str | None = None,
    stress_runner_argv: Sequence[str] | None = None,
) -> dict[str, Any]:
    if official_window_sec is not None and official_window_sec < 0:
        raise ValueError("official_window_sec must be non-negative")

    target_dir = Path(artifact_dir)
    target_dir.mkdir(parents=True, exist_ok=True)

    metrics_path = target_dir / "lol_game.prom"
    server_log_path = target_dir / "server.log"
    runner_output_path = target_dir / "runner.json"
    local_result_path = target_dir / "local_e2e_smoke.json"
    official_window_path = target_dir / "official_window.json"
    runner_delivery_path = target_dir / "runner_delivery_evaluation.json"
    server_accepted_path = target_dir / "server_accepted_delivery_evaluation.json"
    source_dir = target_dir / "_sources"
    summary_source_path = source_dir / "summary.json"
    run_config_source_path = source_dir / "run_config.json"

    started_monotonic = time.monotonic()
    local_result = run_local_e2e_smoke(
        server_argv=server_argv,
        metrics_textfile_path=metrics_path,
        server_log_path=server_log_path,
        runner_output_path=runner_output_path,
        host=host,
        tcp_port=tcp_port,
        participants=participants,
        room_size=room_size,
        timeout_sec=timeout_sec,
        udp_port=udp_port,
        auth_token_prefix=auth_token_prefix,
        start_battle=True,
        loot_smoke=True,
        move_duration_sec=move_duration_sec,
        move_rate_hz=move_rate_hz,
        attack_duration_sec=attack_duration_sec,
        attack_rate_hz=attack_rate_hz,
        battle_cycles=battle_cycles,
        ready_timeout_sec=ready_timeout_sec,
        shutdown_timeout_sec=shutdown_timeout_sec,
        post_runner_metrics_settle_sec=post_runner_metrics_settle_sec,
        server_cwd=server_cwd,
    )
    elapsed_sec = time.monotonic() - started_monotonic
    _write_json(local_result_path, local_result)
    official_window = _official_window_evidence(
        local_valid=bool(local_result["valid"]),
        manual_completed=completed_official_window,
        required_sec=official_window_sec,
        elapsed_sec=elapsed_sec,
    )
    _write_json(official_window_path, official_window)

    gate_evaluations = []
    if local_result["valid"]:
        runner_delivery = evaluate_runner_delivery_from_path(
            runner_output_path,
            min_ratio=runner_delivery_min_ratio,
        )
        server_accepted = evaluate_server_accepted_delivery_from_paths(
            runner_output_path,
            metrics_path,
            min_ratio=runner_delivery_min_ratio,
        )
        _write_json(runner_delivery_path, runner_delivery)
        _write_json(server_accepted_path, server_accepted)
        gate_evaluations = [runner_delivery, server_accepted]

    summary = build_summary(
        run_id=run_id,
        completed_official_window=bool(official_window["completed"]),
        invalid_category="" if local_result["valid"] else "Workload",
        invalid_reason="" if local_result["valid"] else local_result["reason"],
        gate_evaluations=gate_evaluations,
    )
    source_dir.mkdir(parents=True, exist_ok=True)
    _write_json(summary_source_path, summary)
    _write_json(
        run_config_source_path,
        build_run_config(
            run_id=run_id,
            gate_config_path=gate_config_path,
            server=build_execution_target(
                server_argv[0],
                server_argv,
                server_cwd or Path.cwd(),
            ),
            stress_runner=build_execution_target(
                Path(__file__).resolve(),
                stress_runner_argv
                or [sys.executable, "-m", "scripts.release1.official_capacity_run"],
                Path.cwd(),
            ),
        ),
    )

    extras = {
        "runner.json": runner_output_path,
        "lol_game.prom": metrics_path,
        "server.log": server_log_path,
        "local_e2e_smoke.json": local_result_path,
        "official_window.json": official_window_path,
    }
    if local_result["valid"]:
        extras.update(
            {
                "runner_delivery_evaluation.json": runner_delivery_path,
                "server_accepted_delivery_evaluation.json": server_accepted_path,
            }
        )

    manifest = assemble_artifact_bundle(
        summary_path=summary_source_path,
        run_config_path=run_config_source_path,
        artifact_dir=target_dir,
        gate_config_path=gate_config_path,
        extra_artifacts=extras,
    )
    evaluation = load_json(target_dir / "evaluation.json")
    return {
        "schema_version": "release1.official_capacity_smoke.v1",
        "valid": local_result["valid"] and evaluation["result_status"] == "PASS",
        "reason": "" if local_result["valid"] else local_result["reason"],
        "run_id": run_id,
        "artifact_dir": str(target_dir),
        "official_window": official_window,
        "manifest": manifest,
        "evaluation": evaluation,
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Release 1 official-capacity smoke artifact pipeline."
    )
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--artifact-dir", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--tcp-port", required=True, type=int)
    parser.add_argument("--participants", required=True, type=int)
    parser.add_argument("--room-size", required=True, type=int)
    parser.add_argument("--timeout-sec", default=5.0, type=float)
    parser.add_argument("--udp-port", type=int)
    parser.add_argument("--auth-token-prefix")
    parser.add_argument("--completed-official-window", action="store_true")
    parser.add_argument("--official-window-sec", type=float)
    parser.add_argument("--move-duration-sec", default=0.0, type=float)
    parser.add_argument("--move-rate-hz", default=0.0, type=float)
    parser.add_argument("--attack-duration-sec", default=0.0, type=float)
    parser.add_argument("--attack-rate-hz", default=0.0, type=float)
    parser.add_argument("--battle-cycles", default=1, type=int)
    parser.add_argument("--runner-delivery-min-ratio", default=0.99, type=float)
    parser.add_argument(
        "--gate-config",
        default=str(Path(__file__).with_name("gate_config.json")),
    )
    parser.add_argument("--ready-timeout-sec", default=5.0, type=float)
    parser.add_argument("--shutdown-timeout-sec", default=5.0, type=float)
    parser.add_argument("--post-runner-metrics-settle-sec", default=0.0, type=float)
    parser.add_argument("--server-cwd")
    parser.add_argument("server_argv", nargs=argparse.REMAINDER)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = run_official_capacity_smoke(
            run_id=args.run_id,
            artifact_dir=args.artifact_dir,
            server_argv=_server_argv(args.server_argv),
            host=args.host,
            tcp_port=args.tcp_port,
            participants=args.participants,
            room_size=args.room_size,
            timeout_sec=args.timeout_sec,
            udp_port=args.udp_port,
            auth_token_prefix=args.auth_token_prefix,
            completed_official_window=args.completed_official_window,
            official_window_sec=args.official_window_sec,
            move_duration_sec=args.move_duration_sec,
            move_rate_hz=args.move_rate_hz,
            attack_duration_sec=args.attack_duration_sec,
            attack_rate_hz=args.attack_rate_hz,
            battle_cycles=args.battle_cycles,
            runner_delivery_min_ratio=args.runner_delivery_min_ratio,
            gate_config_path=args.gate_config,
            ready_timeout_sec=args.ready_timeout_sec,
            shutdown_timeout_sec=args.shutdown_timeout_sec,
            post_runner_metrics_settle_sec=args.post_runner_metrics_settle_sec,
            server_cwd=args.server_cwd,
            stress_runner_argv=[sys.executable, "-m", "scripts.release1.official_capacity_run"]
            + (argv or sys.argv[1:]),
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"official capacity smoke failed: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, sort_keys=True))
    return 0 if result["valid"] else 1


def _server_argv(values: Sequence[str]) -> list[str]:
    argv = list(values)
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        raise ValueError("server argv is required after --")
    return argv


def _official_window_evidence(
    *,
    local_valid: bool,
    manual_completed: bool,
    required_sec: float | None,
    elapsed_sec: float,
) -> dict[str, Any]:
    if required_sec is None:
        return {
            "schema_version": "release1.official_window.v1",
            "mode": "manual_flag",
            "required_seconds": None,
            "elapsed_seconds": elapsed_sec,
            "completed": local_valid and manual_completed,
        }
    return {
        "schema_version": "release1.official_window.v1",
        "mode": "elapsed",
        "required_seconds": required_sec,
        "elapsed_seconds": elapsed_sec,
        "completed": local_valid and elapsed_sec >= required_sec,
    }


def _write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
