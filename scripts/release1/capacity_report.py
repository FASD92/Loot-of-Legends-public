#!/usr/bin/env python3
"""Evaluate Release 1 capacity-run summary artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence


class ValidationError(ValueError):
    """Raised when a Release 1 artifact does not match its schema."""


DEFAULT_PRIMARY_LATENCY_OP_METRIC_NAMES = {
    "Move": "lol_rudp_move_receive_to_apply_latency_ms",
    "Attack": "lol_rudp_attack_receive_to_apply_latency_ms",
    "Loot claim": "lol_rudp_loot_claim_receive_to_apply_latency_ms",
}

DEFAULT_RUNNER_DELIVERY_COUNTERS = {
    "Move": ("rudp_move_target_sent", "rudp_move_sent"),
    "Attack": ("rudp_attack_target_sent", "rudp_attack_sent"),
    "Loot claim": ("rudp_click_loot_target_sent", "rudp_click_loot_sent"),
}

DEFAULT_SERVER_ACCEPTED_DELIVERY_METRICS = {
    "Move": ("rudp_move_target_sent", "lol_rudp_move_accepted_total"),
    "Attack": ("rudp_attack_target_sent", "lol_rudp_attack_accepted_total"),
    "Loot claim": (
        "rudp_click_loot_server_accept_target_sent",
        "lol_rudp_loot_claim_accepted_total",
    ),
}


def sha256_file(path: Path | str) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_json(path: Path | str) -> dict[str, Any]:
    with Path(path).open(encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValidationError(f"{path}: expected object")
    return data


def extract_latency_bucket_metric(
    metrics_text: str,
    metric_name: str,
) -> tuple[list[tuple[float, int]], int]:
    if metric_name == "":
        raise ValidationError("metric_name must be non-empty")

    bucket_metric_name = f"{metric_name}_bucket"
    count_metric_name = f"{metric_name}_count"
    buckets: list[tuple[float, int]] = []
    total_count = None

    for line_number, raw_line in enumerate(metrics_text.splitlines(), start=1):
        line = raw_line.strip()
        if line == "" or line.startswith("#"):
            continue

        sample_name, sample_value = _split_prometheus_sample(line, line_number)
        if sample_name.startswith(f"{bucket_metric_name}{{"):
            buckets.append(
                (
                    _parse_bucket_upper_bound_ms(
                        sample_name,
                        bucket_metric_name,
                        line_number,
                    ),
                    _parse_prometheus_integer(sample_value, line_number),
                )
            )
        elif sample_name == count_metric_name:
            total_count = _parse_prometheus_integer(sample_value, line_number)

    if not buckets:
        raise ValidationError(f"{metric_name}: missing bucket samples")
    if total_count is None:
        raise ValidationError(f"{metric_name}: missing count")
    return buckets, total_count


def summarize_latency_bucket_metric(
    metrics_text: str,
    metric_name: str,
) -> dict[str, Any]:
    buckets, total_count = extract_latency_bucket_metric(metrics_text, metric_name)
    return {
        "sample_count": total_count,
        "p95_ms": conservative_bucket_percentile_ms(
            buckets,
            percentile=95.0,
            total_count=total_count,
        ),
        "p99_ms": conservative_bucket_percentile_ms(
            buckets,
            percentile=99.0,
            total_count=total_count,
        ),
    }


def evaluate_primary_latency_metric(
    metrics_text: str,
    metric_name: str,
    min_samples: int,
    p99_slo_ms: float,
) -> dict[str, Any]:
    try:
        latency_summary = summarize_latency_bucket_metric(metrics_text, metric_name)
    except ValidationError as error:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason=str(error),
        )

    if latency_summary["sample_count"] < min_samples:
        return _primary_latency_result(
            "INVALID",
            latency_summary,
            invalid_category="Workload",
            reason="insufficient_primary_latency_samples",
        )

    if latency_summary["p99_ms"] is None:
        return _primary_latency_result(
            "INVALID",
            latency_summary,
            invalid_category="Artifact",
            reason="primary_latency_p99_uncovered",
        )

    if latency_summary["p99_ms"] > p99_slo_ms:
        return _primary_latency_result(
            "FAIL",
            latency_summary,
            failed_hard_gate_categories=["Primary latency"],
            reason="primary_latency_p99_exceeded",
        )

    return _primary_latency_result("PASS", latency_summary)


def evaluate_primary_latency_metrics(
    metrics_text: str,
    op_metric_names: Mapping[str, str],
    min_samples: int,
    p99_slo_ms: float,
) -> dict[str, Any]:
    op_results = {
        op_name: evaluate_primary_latency_metric(
            metrics_text,
            metric_name,
            min_samples,
            p99_slo_ms,
        )
        for op_name, metric_name in op_metric_names.items()
    }
    invalid_ops = [
        op_name
        for op_name, result in op_results.items()
        if result["result_status"] == "INVALID"
    ]
    if invalid_ops:
        invalid_category = (
            "Artifact"
            if any(
                op_results[op_name]["invalid_category"] == "Artifact"
                for op_name in invalid_ops
            )
            else "Workload"
        )
        return _primary_latency_result(
            "INVALID",
            invalid_category=invalid_category,
            reason="primary_latency_metric_invalid",
            op_results=op_results,
            invalid_ops=invalid_ops,
        )

    failed_ops = [
        op_name
        for op_name, result in op_results.items()
        if result["result_status"] == "FAIL"
    ]
    if failed_ops:
        return _primary_latency_result(
            "FAIL",
            failed_hard_gate_categories=["Primary latency"],
            reason="primary_latency_p99_exceeded",
            op_results=op_results,
            failed_ops=failed_ops,
        )

    return _primary_latency_result("PASS", op_results=op_results)


def evaluate_primary_latency_metrics_from_path(
    metrics_path: Path | str,
    min_samples: int,
    p99_slo_ms: float,
    op_metric_names: Mapping[str, str] = DEFAULT_PRIMARY_LATENCY_OP_METRIC_NAMES,
) -> dict[str, Any]:
    return evaluate_primary_latency_metrics(
        Path(metrics_path).read_text(encoding="utf-8"),
        op_metric_names,
        min_samples=min_samples,
        p99_slo_ms=p99_slo_ms,
    )


def evaluate_runner_delivery(
    runner_result: Mapping[str, Any],
    min_ratio: float,
    delivery_counters: Mapping[str, tuple[str, str]] = DEFAULT_RUNNER_DELIVERY_COUNTERS,
) -> dict[str, Any]:
    if not math.isfinite(min_ratio) or min_ratio <= 0 or min_ratio > 1:
        raise ValidationError("runner delivery min ratio must be greater than 0 and <= 1")

    op_results: dict[str, dict[str, Any]] = {}
    missing_ops: list[str] = []
    malformed_ops: list[str] = []
    zero_target_ops: list[str] = []
    actual_exceeds_target_ops: list[str] = []
    failed_ops: list[str] = []

    for op_name, (target_counter, actual_counter) in delivery_counters.items():
        if target_counter not in runner_result or actual_counter not in runner_result:
            missing_ops.append(op_name)
            continue

        try:
            target_count = _runner_delivery_counter_value(
                runner_result[target_counter],
                target_counter,
            )
            actual_count = _runner_delivery_counter_value(
                runner_result[actual_counter],
                actual_counter,
            )
        except ValidationError:
            malformed_ops.append(op_name)
            continue

        if target_count == 0:
            zero_target_ops.append(op_name)
            continue
        if actual_count > target_count:
            actual_exceeds_target_ops.append(op_name)
            continue

        delivery_ratio = actual_count / target_count
        op_results[op_name] = {
            "target_sent": target_count,
            "actual_sent": actual_count,
            "delivery_ratio": delivery_ratio,
        }
        if delivery_ratio < min_ratio:
            failed_ops.append(op_name)

    if missing_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason="runner_delivery_counter_missing",
            op_results=op_results,
            invalid_ops=missing_ops,
        )
    if malformed_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason="runner_delivery_counter_malformed",
            op_results=op_results,
            invalid_ops=malformed_ops,
        )
    if actual_exceeds_target_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason="runner_delivery_actual_exceeds_target",
            op_results=op_results,
            invalid_ops=actual_exceeds_target_ops,
        )
    if zero_target_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Workload",
            reason="runner_delivery_target_missing",
            op_results=op_results,
            invalid_ops=zero_target_ops,
        )
    if failed_ops:
        return _primary_latency_result(
            "FAIL",
            failed_hard_gate_categories=["Correctness"],
            reason="runner_delivery_ratio_below_min",
            op_results=op_results,
            failed_ops=failed_ops,
        )
    return _primary_latency_result("PASS", op_results=op_results)


def evaluate_runner_delivery_from_path(
    runner_result_path: Path | str,
    min_ratio: float,
) -> dict[str, Any]:
    return evaluate_runner_delivery(
        load_json(runner_result_path),
        min_ratio=min_ratio,
    )


def evaluate_server_accepted_delivery(
    runner_result: Mapping[str, Any],
    metrics_text: str,
    min_ratio: float,
    accepted_metrics: Mapping[
        str,
        tuple[str, str],
    ] = DEFAULT_SERVER_ACCEPTED_DELIVERY_METRICS,
) -> dict[str, Any]:
    if not math.isfinite(min_ratio) or min_ratio <= 0 or min_ratio > 1:
        raise ValidationError("accepted delivery min ratio must be greater than 0 and <= 1")

    op_results: dict[str, dict[str, Any]] = {}
    missing_target_ops: list[str] = []
    malformed_target_ops: list[str] = []
    missing_metric_ops: list[str] = []
    malformed_metric_ops: list[str] = []
    zero_target_ops: list[str] = []
    accepted_exceeds_target_ops: list[str] = []
    failed_ops: list[str] = []

    for op_name, (target_counter, metric_name) in accepted_metrics.items():
        if target_counter not in runner_result:
            missing_target_ops.append(op_name)
            continue

        try:
            target_count = _runner_delivery_counter_value(
                runner_result[target_counter],
                target_counter,
            )
        except ValidationError:
            malformed_target_ops.append(op_name)
            continue

        try:
            accepted_count = extract_prometheus_counter_metric(
                metrics_text,
                metric_name,
            )
        except ValidationError as error:
            if "missing sample" in str(error):
                missing_metric_ops.append(op_name)
            else:
                malformed_metric_ops.append(op_name)
            continue

        if target_count == 0:
            zero_target_ops.append(op_name)
            continue
        if accepted_count > target_count:
            accepted_exceeds_target_ops.append(op_name)
            continue

        accepted_ratio = accepted_count / target_count
        op_results[op_name] = {
            "target_sent": target_count,
            "accepted_count": accepted_count,
            "accepted_ratio": accepted_ratio,
        }
        if accepted_ratio < min_ratio:
            failed_ops.append(op_name)

    if missing_target_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason="server_accepted_delivery_target_counter_missing",
            op_results=op_results,
            invalid_ops=missing_target_ops,
        )
    if malformed_target_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason="server_accepted_delivery_target_counter_malformed",
            op_results=op_results,
            invalid_ops=malformed_target_ops,
        )
    if missing_metric_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason="server_accepted_delivery_metric_missing",
            op_results=op_results,
            invalid_ops=missing_metric_ops,
        )
    if malformed_metric_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason="server_accepted_delivery_metric_malformed",
            op_results=op_results,
            invalid_ops=malformed_metric_ops,
        )
    if accepted_exceeds_target_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Artifact",
            reason="server_accepted_delivery_metric_exceeds_target",
            op_results=op_results,
            invalid_ops=accepted_exceeds_target_ops,
        )
    if zero_target_ops:
        return _primary_latency_result(
            "INVALID",
            invalid_category="Workload",
            reason="server_accepted_delivery_target_missing",
            op_results=op_results,
            invalid_ops=zero_target_ops,
        )
    if failed_ops:
        return _primary_latency_result(
            "FAIL",
            failed_hard_gate_categories=["Correctness"],
            reason="server_accepted_delivery_ratio_below_min",
            op_results=op_results,
            failed_ops=failed_ops,
        )
    return _primary_latency_result("PASS", op_results=op_results)


def evaluate_server_accepted_delivery_from_paths(
    runner_result_path: Path | str,
    metrics_path: Path | str,
    min_ratio: float,
) -> dict[str, Any]:
    return evaluate_server_accepted_delivery(
        load_json(runner_result_path),
        Path(metrics_path).read_text(encoding="utf-8"),
        min_ratio=min_ratio,
    )


def conservative_bucket_percentile_ms(
    buckets: Sequence[tuple[float, int]],
    percentile: float,
    total_count: int,
) -> float | None:
    if percentile <= 0 or percentile > 100:
        raise ValidationError(
            "percentile must be greater than 0 and less than or equal to 100"
        )
    if total_count < 0:
        raise ValidationError("total_count must be non-negative")
    if total_count == 0:
        return None

    target_rank = math.ceil(total_count * percentile / 100.0)
    percentile_upper_bound = None
    previous_count = 0
    previous_bound = -math.inf
    for upper_bound_ms, cumulative_count in buckets:
        if upper_bound_ms < 0:
            raise ValidationError("bucket upper bound must be non-negative")
        if upper_bound_ms < previous_bound:
            raise ValidationError("bucket upper bounds must be non-decreasing")
        if cumulative_count < 0:
            raise ValidationError("bucket count must be non-negative")
        if cumulative_count < previous_count:
            raise ValidationError("bucket counts must be non-monotonic")
        if percentile_upper_bound is None and cumulative_count >= target_rank:
            percentile_upper_bound = float(upper_bound_ms)
        previous_bound = upper_bound_ms
        previous_count = cumulative_count
    return percentile_upper_bound


def _primary_latency_result(
    result_status: str,
    latency_summary: dict[str, Any] | None = None,
    *,
    invalid_category: str = "",
    failed_hard_gate_categories: list[str] | None = None,
    reason: str = "",
    **extra_fields: Any,
) -> dict[str, Any]:
    return {
        "result_status": result_status,
        "invalid_category": invalid_category,
        "failed_hard_gate_categories": failed_hard_gate_categories or [],
        "reason": reason,
        **(latency_summary or {}),
        **extra_fields,
    }


def _split_prometheus_sample(line: str, line_number: int) -> tuple[str, str]:
    parts = line.rsplit(maxsplit=1)
    if len(parts) != 2:
        raise ValidationError(f"line {line_number}: malformed metric sample")
    return parts[0], parts[1]


def _parse_bucket_upper_bound_ms(
    sample_name: str,
    bucket_metric_name: str,
    line_number: int,
) -> float:
    labels = sample_name[len(bucket_metric_name) :]
    if not labels.startswith('{le="') or not labels.endswith('"}'):
        raise ValidationError(f"line {line_number}: malformed le label")

    try:
        upper_bound_ms = float(labels[len('{le="') : -len('"}')])
    except ValueError as error:
        raise ValidationError(f"line {line_number}: malformed le label") from error
    if not math.isfinite(upper_bound_ms) or upper_bound_ms < 0:
        raise ValidationError(f"line {line_number}: malformed le label")
    return upper_bound_ms


def _parse_prometheus_integer(value: str, line_number: int) -> int:
    try:
        parsed = float(value)
    except ValueError as error:
        raise ValidationError(f"line {line_number}: expected integer value") from error
    if not math.isfinite(parsed) or parsed < 0 or not parsed.is_integer():
        raise ValidationError(f"line {line_number}: expected integer value")
    return int(parsed)


def extract_prometheus_counter_metric(metrics_text: str, metric_name: str) -> int:
    if metric_name == "":
        raise ValidationError("metric_name must be non-empty")

    found_value = None
    for line_number, raw_line in enumerate(metrics_text.splitlines(), start=1):
        line = raw_line.strip()
        if line == "" or line.startswith("#"):
            continue
        sample_name, sample_value = _split_prometheus_sample(line, line_number)
        if sample_name != metric_name:
            continue
        if found_value is not None:
            raise ValidationError(f"{metric_name}: duplicate sample")
        found_value = _parse_prometheus_integer(sample_value, line_number)

    if found_value is None:
        raise ValidationError(f"{metric_name}: missing sample")
    return found_value


def _runner_delivery_counter_value(value: Any, counter_name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValidationError(f"{counter_name}: expected non-negative integer")
    return value


def validate_schema(data: Any, schema: dict[str, Any], path: str = "$") -> None:
    _validate_schema(data, schema, schema, path)


def _validate_schema(
    data: Any,
    schema: dict[str, Any],
    root_schema: dict[str, Any],
    path: str,
) -> None:
    if "$ref_local" in schema:
        definitions = root_schema.get("definitions", {})
        name = schema["$ref_local"]
        if name not in definitions:
            raise ValidationError(f"{path}: missing local schema definition {name}")
        _validate_schema(data, definitions[name], root_schema, path)
        return

    if "const" in schema and data != schema["const"]:
        raise ValidationError(f"{path}: expected {schema['const']!r}")

    if "enum" in schema and data not in schema["enum"]:
        raise ValidationError(f"{path}: expected one of {schema['enum']!r}")

    type_spec = schema.get("type")
    if type_spec is not None and not _matches_type(data, type_spec):
        raise ValidationError(f"{path}: expected {_format_type(type_spec)}")

    if isinstance(data, dict):
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        for key in required:
            if key not in data:
                raise ValidationError(f"{path}: missing required field {key}")

        additional = schema.get("additionalProperties", True)
        for key, value in data.items():
            child_path = f"{path}.{key}"
            if key in properties:
                _validate_schema(value, properties[key], root_schema, child_path)
            elif additional is False:
                raise ValidationError(f"{path}: unexpected field {key}")
            elif isinstance(additional, dict):
                _validate_schema(value, additional, root_schema, child_path)

    if isinstance(data, list) and "items" in schema:
        for index, value in enumerate(data):
            _validate_schema(value, schema["items"], root_schema, f"{path}[{index}]")

    if isinstance(data, str) and "minLength" in schema:
        if len(data) < int(schema["minLength"]):
            raise ValidationError(f"{path}: expected at least {schema['minLength']} chars")


def _matches_type(data: Any, type_spec: Any) -> bool:
    if isinstance(type_spec, list):
        return any(_matches_type(data, item) for item in type_spec)
    if type_spec == "object":
        return isinstance(data, dict)
    if type_spec == "array":
        return isinstance(data, list)
    if type_spec == "string":
        return isinstance(data, str)
    if type_spec == "boolean":
        return isinstance(data, bool)
    if type_spec == "integer":
        return isinstance(data, int) and not isinstance(data, bool)
    if type_spec == "number":
        return isinstance(data, (int, float)) and not isinstance(data, bool)
    raise ValidationError(f"unsupported schema type {type_spec!r}")


def _format_type(type_spec: Any) -> str:
    if isinstance(type_spec, list):
        return " or ".join(str(item) for item in type_spec)
    return str(type_spec)


def evaluate_capacity_run(
    summary: dict[str, Any],
    run_config: dict[str, Any],
    gate_config_sha256: str,
) -> dict[str, Any]:
    result = {
        "result_status": "PASS",
        "invalid_category": "",
        "failed_hard_gate_categories": [],
        "regression_comparisons": _evaluate_regression_comparisons(
            summary.get("regression_comparisons", []),
            gate_config_sha256,
        ),
    }

    if run_config.get("gate_config_sha256") != gate_config_sha256:
        result.update(
            {
                "result_status": "INVALID",
                "invalid_category": "Artifact",
                "reason": "gate_config_checksum_mismatch",
            }
        )
        return result

    validity = summary.get("validity", {})
    if not validity.get("valid", False):
        result.update(
            {
                "result_status": "INVALID",
                "invalid_category": validity.get("invalid_category", ""),
                "reason": validity.get("reason", ""),
            }
        )
        return result

    hard_gates = summary.get("hard_gates", {})
    if not hard_gates.get("all_passed", False):
        result.update(
            {
                "result_status": "FAIL",
                "failed_hard_gate_categories": hard_gates.get("failed_categories", []),
            }
        )
        return result

    if summary.get("operator_aborted", False) or not summary.get(
        "completed_official_window",
        False,
    ):
        result["result_status"] = "ABORTED"

    return result


def _evaluate_regression_comparisons(
    comparisons: list[dict[str, Any]],
    gate_config_sha256: str,
) -> list[dict[str, Any]]:
    results = []
    for comparison in comparisons:
        matches = comparison.get("gate_config_sha256") == gate_config_sha256
        results.append(
            {
                "name": comparison.get("name", ""),
                "valid": matches,
                "skipped_reason": "" if matches else "gate_config_checksum_mismatch",
            }
        )
    return results


def evaluate_from_paths(
    summary_path: Path | str,
    run_config_path: Path | str,
    gate_config_path: Path | str,
) -> dict[str, Any]:
    summary = load_json(summary_path)
    run_config = load_json(run_config_path)
    summary_schema = load_json(
        Path(__file__).with_name("schemas") / "summary.schema.json"
    )
    run_config_schema = load_json(
        Path(__file__).with_name("schemas") / "run_config.schema.json"
    )

    validate_schema(summary, summary_schema)
    validate_schema(run_config, run_config_schema)
    return evaluate_capacity_run(summary, run_config, sha256_file(gate_config_path))


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Evaluate Release 1 capacity-run summary artifacts."
    )
    parser.add_argument("--summary", help="Path to summary.json.")
    parser.add_argument("--run-config", help="Path to run_config.json.")
    parser.add_argument(
        "--gate-config",
        default=str(Path(__file__).with_name("gate_config.json")),
        help="Path to repository-versioned gate_config.json.",
    )
    parser.add_argument(
        "--primary-latency-metrics",
        help="Path to Prometheus textfile with Release1 primary latency metrics.",
    )
    parser.add_argument(
        "--primary-latency-min-samples",
        type=int,
        default=100000,
        help="Minimum accepted primary latency samples required per op.",
    )
    parser.add_argument(
        "--primary-latency-p99-slo-ms",
        type=float,
        default=50.0,
        help="Primary latency p99 SLO in milliseconds.",
    )
    parser.add_argument(
        "--runner-result",
        help="Path to external runner JSON with target and actual delivery counters.",
    )
    parser.add_argument(
        "--server-accepted-metrics",
        help=(
            "Path to Prometheus textfile with server accepted counters. "
            "Requires --runner-result."
        ),
    )
    parser.add_argument(
        "--runner-delivery-min-ratio",
        type=float,
        default=0.99,
        help="Minimum accepted runner delivery ratio.",
    )
    args = parser.parse_args(argv)
    if args.server_accepted_metrics and not args.runner_result:
        parser.error("--server-accepted-metrics requires --runner-result")

    if args.runner_result:
        if args.summary or args.run_config or args.primary_latency_metrics:
            parser.error(
                "--summary, --run-config, and --primary-latency-metrics cannot be "
                "used with --runner-result"
            )
        if args.runner_delivery_min_ratio <= 0 or args.runner_delivery_min_ratio > 1:
            parser.error("--runner-delivery-min-ratio must be greater than 0 and <= 1")
        return args

    if args.primary_latency_metrics:
        if args.summary or args.run_config:
            parser.error(
                "--summary and --run-config cannot be used with "
                "--primary-latency-metrics"
            )
        if args.primary_latency_min_samples <= 0:
            parser.error("--primary-latency-min-samples must be positive")
        if args.primary_latency_p99_slo_ms <= 0:
            parser.error("--primary-latency-p99-slo-ms must be positive")
        return args

    if not args.summary:
        parser.error(
            "--summary is required unless --primary-latency-metrics or "
            "--runner-result is set"
        )
    if not args.run_config:
        parser.error(
            "--run-config is required unless --primary-latency-metrics or "
            "--runner-result is set"
        )
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.primary_latency_metrics:
            result = evaluate_primary_latency_metrics_from_path(
                args.primary_latency_metrics,
                min_samples=args.primary_latency_min_samples,
                p99_slo_ms=args.primary_latency_p99_slo_ms,
            )
        elif args.runner_result:
            if args.server_accepted_metrics:
                result = evaluate_server_accepted_delivery_from_paths(
                    args.runner_result,
                    args.server_accepted_metrics,
                    min_ratio=args.runner_delivery_min_ratio,
                )
            else:
                result = evaluate_runner_delivery_from_path(
                    args.runner_result,
                    min_ratio=args.runner_delivery_min_ratio,
                )
        else:
            result = evaluate_from_paths(args.summary, args.run_config, args.gate_config)
    except (OSError, json.JSONDecodeError, ValidationError) as error:
        print(f"capacity report evaluation failed: {error}", file=sys.stderr)
        return 2

    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
