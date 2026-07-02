#!/usr/bin/env python3
"""Run one Release 1 concurrent active-room capacity probe."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Any, Callable, Sequence

from scripts.release1.capacity_report import (
    DEFAULT_PRIMARY_LATENCY_OP_METRIC_NAMES,
    evaluate_runner_delivery,
    evaluate_primary_latency_metrics,
    evaluate_server_accepted_delivery,
)
from scripts.release1.external_stress_runner import run_external_tcp_room_fill
from scripts.release1.server_process import _metrics_ready, _stop_process


SCHEMA_VERSION = "release1.concurrent_capacity_probe.v1"
MAX_RUDP_CLIENT_ID = 0xFFFFFFFF
RunnerGroup = Callable[..., dict[str, Any]]


def run_concurrent_runner_groups(
    *,
    artifact_dir: Path | str,
    host: str,
    tcp_port: int,
    participants: int,
    room_size: int,
    timeout_sec: float,
    udp_port: int | None = None,
    auth_token_prefix: str | None = None,
    move_duration_sec: float = 0.0,
    move_rate_hz: float = 0.0,
    attack_duration_sec: float = 0.0,
    attack_rate_hz: float = 0.0,
    battle_cycles: int = 1,
    first_client_id: int = 100000,
    client_id_stride: int = 1000,
    rudp_handshake_settle_sec: float = 0.0,
    max_workers: int | None = None,
    run_group: RunnerGroup = run_external_tcp_room_fill,
) -> dict[str, Any]:
    group_count = _validate_group_config(
        participants,
        room_size,
        timeout_sec,
        first_client_id,
        client_id_stride,
        max_workers,
        auth_token_prefix,
    )
    root = Path(artifact_dir)
    root.mkdir(parents=True, exist_ok=True)

    def run_one(group_index: int) -> dict[str, Any]:
        try:
            result = run_group(
                host=host,
                tcp_port=tcp_port,
                participants=room_size,
                room_size=room_size,
                timeout_sec=timeout_sec,
                udp_port=udp_port,
                auth_token_prefix=_group_auth_token_prefix(auth_token_prefix, group_index),
                start_battle=True,
                loot_smoke=True,
                move_duration_sec=move_duration_sec,
                move_rate_hz=move_rate_hz,
                attack_duration_sec=attack_duration_sec,
                attack_rate_hz=attack_rate_hz,
                battle_cycles=battle_cycles,
                client_id_base=first_client_id + (group_index * client_id_stride),
                rudp_handshake_settle_sec=rudp_handshake_settle_sec,
            )
        except BaseException as error:
            result = {
                "valid": False,
                "reason": f"{type(error).__name__}: {error}",
            }
        result["group"] = group_index
        _write_json(root / f"runner_group_{group_index:03d}.json", result)
        return result

    worker_count = max_workers if max_workers is not None else group_count
    with concurrent.futures.ThreadPoolExecutor(max_workers=worker_count) as executor:
        futures = [
            executor.submit(run_one, group_index)
            for group_index in range(group_count)
        ]
        group_results = [
            future.result()
            for future in concurrent.futures.as_completed(futures)
        ]

    group_results.sort(key=lambda result: int(result.get("group", -1)))
    return {
        "group_count": group_count,
        "group_results": group_results,
        "valid_groups": sum(1 for result in group_results if result.get("valid")),
        "group_reason_counts": dict(
            Counter(str(result.get("reason", "")) for result in group_results)
        ),
        "aggregate_runner": aggregate_runner_results(group_results),
    }


def aggregate_runner_results(group_results: Sequence[dict[str, Any]]) -> dict[str, Any]:
    counter_names = [
        "rudp_move_target_sent",
        "rudp_move_sent",
        "rudp_attack_target_sent",
        "rudp_attack_sent",
        "rudp_click_loot_target_sent",
        "rudp_click_loot_sent",
        "rudp_click_loot_server_accept_target_sent",
        "tcp_arena_gameplay_start_received",
        "tcp_battle_final_ranking_received",
        "tcp_lobby_return_received",
    ]
    aggregate = {
        name: sum(int(result.get(name, 0) or 0) for result in group_results)
        for name in counter_names
    }
    aggregate["valid"] = bool(group_results) and all(
        bool(result.get("valid")) for result in group_results
    )
    aggregate["reason"] = "" if aggregate["valid"] else "group_failed"
    return aggregate


def run_concurrent_capacity_probe(
    *,
    artifact_dir: Path | str,
    server_argv: Sequence[str],
    host: str,
    tcp_port: int,
    participants: int,
    room_size: int,
    timeout_sec: float,
    udp_port: int | None = None,
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
    poll_interval_sec: float = 0.05,
    post_runner_metrics_settle_sec: float = 0.0,
    max_workers: int | None = None,
    server_cwd: Path | str | None = None,
    run_group: RunnerGroup = run_external_tcp_room_fill,
) -> dict[str, Any]:
    if not server_argv:
        raise ValueError("server argv is required")
    if primary_latency_min_samples <= 0:
        raise ValueError("primary_latency_min_samples must be positive")
    if primary_latency_p99_slo_ms <= 0:
        raise ValueError("primary_latency_p99_slo_ms must be positive")

    root = Path(artifact_dir)
    root.mkdir(parents=True, exist_ok=True)
    metrics_path = root / "lol_game.prom"
    server_log_path = root / "server.log"
    probe_path = root / "probe.json"
    if metrics_path.exists():
        metrics_path.unlink()

    result: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "artifact_dir": str(root),
        "metrics_textfile_path": str(metrics_path),
        "server_log_path": str(server_log_path),
        "participants": participants,
        "room_size": room_size,
        "timeout_sec": timeout_sec,
        "rudp_handshake_settle_sec": rudp_handshake_settle_sec,
        "primary_latency_min_samples": primary_latency_min_samples,
        "primary_latency_p99_slo_ms": primary_latency_p99_slo_ms,
    }
    server_state: dict[str, Any] = {
        "started": False,
        "ready": False,
        "stopped": False,
        "terminated": False,
        "killed": False,
        "return_code": None,
        "reason": "",
    }

    with server_log_path.open("wb") as log_file:
        process = subprocess.Popen(  # noqa: S603 - direct argv, no shell.
            list(server_argv),
            cwd=str(server_cwd) if server_cwd is not None else None,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
        server_state["started"] = True
        started = time.monotonic()
        try:
            server_state["ready"] = _wait_until_ready(
                process,
                metrics_path,
                ready_timeout_sec,
                poll_interval_sec,
            )
            if not server_state["ready"]:
                server_state["reason"] = "metrics_ready_timeout_or_process_exit"
                result["valid"] = False
                result["reason"] = server_state["reason"]
                return result

            group_result = run_concurrent_runner_groups(
                artifact_dir=root,
                host=host,
                tcp_port=tcp_port,
                participants=participants,
                room_size=room_size,
                timeout_sec=timeout_sec,
                udp_port=udp_port,
                auth_token_prefix=auth_token_prefix,
                move_duration_sec=move_duration_sec,
                move_rate_hz=move_rate_hz,
                attack_duration_sec=attack_duration_sec,
                attack_rate_hz=attack_rate_hz,
                battle_cycles=battle_cycles,
                first_client_id=first_client_id,
                client_id_stride=client_id_stride,
                rudp_handshake_settle_sec=rudp_handshake_settle_sec,
                max_workers=max_workers,
                run_group=run_group,
            )
            result.update(group_result)
            if post_runner_metrics_settle_sec > 0:
                time.sleep(post_runner_metrics_settle_sec)

            metrics_text = metrics_path.read_text(encoding="utf-8")
            aggregate = result["aggregate_runner"]
            result["runner_delivery_evaluation"] = evaluate_runner_delivery(
                aggregate,
                min_ratio=runner_delivery_min_ratio,
            )
            result["server_accepted_delivery_evaluation"] = evaluate_server_accepted_delivery(
                aggregate,
                metrics_text,
                min_ratio=runner_delivery_min_ratio,
            )
            result["primary_latency_evaluation"] = evaluate_primary_latency_metrics(
                metrics_text,
                DEFAULT_PRIMARY_LATENCY_OP_METRIC_NAMES,
                min_samples=primary_latency_min_samples,
                p99_slo_ms=primary_latency_p99_slo_ms,
            )
            runner_delivery_passed = (
                result["runner_delivery_evaluation"].get("result_status") == "PASS"
            )
            server_accepted_delivery_passed = (
                result["server_accepted_delivery_evaluation"].get("result_status") == "PASS"
            )
            primary_latency_status = result["primary_latency_evaluation"].get(
                "result_status",
            )
            result["valid"] = (
                bool(aggregate["valid"]) and
                runner_delivery_passed and
                server_accepted_delivery_passed and
                primary_latency_status == "PASS"
            )
            if result["valid"]:
                result["reason"] = ""
            elif (
                bool(aggregate["valid"]) and
                runner_delivery_passed and
                server_accepted_delivery_passed and
                primary_latency_status == "INVALID"
            ):
                result["reason"] = "primary_latency_invalid"
            elif (
                bool(aggregate["valid"]) and
                runner_delivery_passed and
                server_accepted_delivery_passed and
                primary_latency_status == "FAIL"
            ):
                result["reason"] = "primary_latency_failed"
            else:
                result["reason"] = "group_or_delivery_failed"
            result["evaluation"] = {
                "result_status": "PASS" if result["valid"] else "FAIL",
                "reason": result["reason"],
            }
            return result
        finally:
            _stop_process(process, server_state, shutdown_timeout_sec)
            result["server"] = server_state
            result["elapsed_sec"] = round(time.monotonic() - started, 3)
            if "evaluation" not in result:
                result["evaluation"] = {
                    "result_status": "PASS" if result.get("valid") else "INVALID",
                    "reason": str(result.get("reason", "")),
                }
            _write_json(probe_path, result)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run one Release 1 concurrent active-room capacity probe."
    )
    parser.add_argument("--artifact-dir", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--tcp-port", required=True, type=int)
    parser.add_argument("--participants", required=True, type=int)
    parser.add_argument("--room-size", default=10, type=int)
    parser.add_argument("--timeout-sec", default=5.0, type=float)
    parser.add_argument("--udp-port", type=int)
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
    parser.add_argument("--poll-interval-sec", default=0.05, type=float)
    parser.add_argument("--post-runner-metrics-settle-sec", default=0.0, type=float)
    parser.add_argument("--max-workers", type=int)
    parser.add_argument("--server-cwd")
    parser.add_argument("server_argv", nargs=argparse.REMAINDER)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = run_concurrent_capacity_probe(
            artifact_dir=args.artifact_dir,
            server_argv=_server_argv(args.server_argv),
            host=args.host,
            tcp_port=args.tcp_port,
            participants=args.participants,
            room_size=args.room_size,
            timeout_sec=args.timeout_sec,
            udp_port=args.udp_port,
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
            poll_interval_sec=args.poll_interval_sec,
            post_runner_metrics_settle_sec=args.post_runner_metrics_settle_sec,
            max_workers=args.max_workers,
            server_cwd=args.server_cwd,
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"concurrent capacity probe failed: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, sort_keys=True))
    return 0 if result["valid"] else 1


def _validate_group_config(
    participants: int,
    room_size: int,
    timeout_sec: float,
    first_client_id: int,
    client_id_stride: int,
    max_workers: int | None,
    auth_token_prefix: str | None,
) -> int:
    if participants <= 0:
        raise ValueError("participants must be positive")
    if room_size <= 0:
        raise ValueError("room_size must be positive")
    if participants % room_size != 0:
        raise ValueError("participants must be divisible by room_size")
    if timeout_sec <= 0:
        raise ValueError("timeout_sec must be positive")
    if first_client_id <= 0:
        raise ValueError("first_client_id must be positive")
    if client_id_stride < room_size:
        raise ValueError("client_id_stride must be greater than or equal to room_size")
    if max_workers is not None and max_workers <= 0:
        raise ValueError("max_workers must be positive")
    if auth_token_prefix is not None and not auth_token_prefix.strip():
        raise ValueError("auth_token_prefix must not be blank")

    group_count = participants // room_size
    last_client_id = first_client_id + ((group_count - 1) * client_id_stride) + room_size - 1
    if last_client_id > MAX_RUDP_CLIENT_ID:
        raise ValueError("client id range must fit in uint32")
    return group_count


def _group_auth_token_prefix(prefix: str | None, group_index: int) -> str | None:
    if prefix is None:
        return None
    return f"{prefix}G{group_index:03d}"


def _wait_until_ready(
    process: subprocess.Popen[bytes],
    metrics_path: Path,
    ready_timeout_sec: float,
    poll_interval_sec: float,
) -> bool:
    deadline = time.monotonic() + ready_timeout_sec
    while time.monotonic() < deadline:
        if _metrics_ready(metrics_path):
            return True
        if process.poll() is not None:
            return False
        time.sleep(poll_interval_sec)
    return False


def _server_argv(values: Sequence[str]) -> list[str]:
    argv = list(values)
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        raise ValueError("server argv is required after --")
    return argv


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, sort_keys=True, indent=2), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
