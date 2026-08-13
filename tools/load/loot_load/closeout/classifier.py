"""Deterministic, socket-free classification of immutable run facts."""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence

from ..evidence.contracts import canonical_json_bytes, validate_document
from ..evidence.fact_writer import write_json_atomic


_SAMPLES = (
    "critical_terminal_latency_ms",
    "room_queue_delay_ms",
    "room_processing_duration_ms",
    "active_snapshot_interval_ms",
    "durable_append_latency_ms",
    "process_cpu_busy_ratio",
    "process_rss_bytes",
    "process_fd_count",
    "outbox_oldest_pending_ms",
    "outbox_drain_ms",
    "gameplay_progress_gap_ms",
    "worker_heartbeat_gap_ms",
    "scheduler_slip_ms",
)
_VALIDITY_ORDER = (
        "PROVENANCE_MISSING",
        "PROVENANCE_MISMATCH",
        "ARTIFACT_DIGEST_INVALID",
        "SERIES_IDENTITY_MISMATCH",
        "ENVIRONMENT_MISMATCH",
        "WORKER_HEARTBEAT_GAP",
        "WORKER_CRASHED",
        "IPC_QUEUE_SATURATED",
        "PARTICIPANT_COUNT_MISMATCH",
        "ROOM_TOPOLOGY_MISMATCH",
        "COMMAND_RESULT_TIMEOUT",
        "SERVER_INVARIANT_VIOLATION",
        "SAMPLE_COVERAGE_INSUFFICIENT",
        "METRICS_COVERAGE_INSUFFICIENT",
        "FACT_STREAM_CORRUPT",
        "CLOCK_INVALID",
        "SOURCE_DIRTY",
        "PROFILE_INVALID",
        "SCHEDULER_SLIP_EXCEEDED",
        "COMMAND_EMISSION_COVERAGE_LOW",
)
_VALIDITY_REASONS = frozenset(_VALIDITY_ORDER)
_CORRECTNESS_ORDER = (
        "RANKING_MISMATCH",
        "CAPTURED_PARTICIPANT_MISMATCH",
        "RECIPIENT_MISMATCH",
        "RESULT_BEFORE_DURABILITY",
        "ROOM_OPEN_BEFORE_DURABILITY",
        "REMATCH_FAILED",
        "STALE_GENERATION_MUTATION",
        "DROP_EXACTLY_ONCE_VIOLATION",
    "SETTLEMENT_AT_MOST_ONCE_VIOLATION",
    "SETTLEMENT_HASH_CONFLICT",
    "CANONICAL_WORKLOAD_INCOMPLETE",
    "GAMEPLAY_PROGRESS_STALLED",
)
_CORRECTNESS_REASONS = frozenset(_CORRECTNESS_ORDER)
_PACKAGE_ORDER = (
        "PACKAGE_FILE_MISSING",
        "PACKAGE_CHECKSUM_MISMATCH",
        "PACKAGE_SANITIZATION_FAILED",
        "PACKAGE_MANIFEST_INVALID",
)
_PACKAGE_REASONS = frozenset(_PACKAGE_ORDER)
_REASON_ORDER = (
    "OPERATOR_ABORTED",
    *_VALIDITY_ORDER,
    *_CORRECTNESS_ORDER,
    "CRITICAL_LATENCY_EXCEEDED",
    "ROOM_QUEUE_DELAY_EXCEEDED",
    "SNAPSHOT_GAP_EXCEEDED",
    "DURABLE_APPEND_LATENCY_EXCEEDED",
    "CPU_LIMIT_EXCEEDED",
    "MEMORY_LIMIT_EXCEEDED",
    "FILE_DESCRIPTOR_LIMIT_EXCEEDED",
    "OUTBOX_AGE_EXCEEDED",
    "OUTBOX_DRAIN_TIMEOUT",
    *_PACKAGE_ORDER,
)
_REASON_RANK = {reason: rank for rank, reason in enumerate(_REASON_ORDER)}


@dataclass(frozen=True)
class EvaluationFacts:
    operator_aborted: bool
    package_status: str
    package_reason_codes: tuple[str, ...]
    validity_reason_codes: tuple[str, ...]
    correctness_reason_codes: tuple[str, ...]
    samples: Mapping[str, Sequence[float]]
    expected_metric_scrapes: int
    successful_metric_scrapes: int
    planned_commands: int
    emitted_commands: int
    measurement_duration_ms: int
    memory_limit_bytes: int
    file_descriptor_limit: int


def classify(run_input: dict[str, Any], facts: EvaluationFacts) -> dict[str, Any]:
    errors = validate_document("run-input", run_input)
    if errors:
        raise ValueError("run input is invalid: " + "; ".join(errors))
    samples = _validate_facts(facts)
    statistics = _statistics(facts, samples)
    validity = list(facts.validity_reason_codes)
    correctness = list(facts.correctness_reason_codes)

    if any(run_input["provenance"][owner]["sourceDirty"] for owner in ("game", "meta", "harness")):
        validity.append("SOURCE_DIRTY")
    required_duration_ms = max(0, run_input["profile"]["measurementSeconds"] - 1) * 1_000
    if facts.measurement_duration_ms < required_duration_ms:
        validity.append("PROFILE_INVALID")
    if statistics["ratios"]["metrics_coverage"] < 0.99:
        validity.append("METRICS_COVERAGE_INSUFFICIENT")
    if statistics["ratios"]["command_emission_coverage"] < 0.995:
        validity.append("COMMAND_EMISSION_COVERAGE_LOW")
    if _maximum(samples["worker_heartbeat_gap_ms"]) > 5_000:
        validity.append("WORKER_HEARTBEAT_GAP")
    if (
        _percentile(samples["scheduler_slip_ms"], 0.99) > 100
        or _maximum(samples["scheduler_slip_ms"]) > 500
    ):
        validity.append("SCHEDULER_SLIP_EXCEEDED")
    if run_input["profile"]["gateClass"] in {
        "OFFICIAL_CAPACITY",
        "FROZEN_REGRESSION_BASELINE",
    } and (
        len(samples["critical_terminal_latency_ms"]) < 10_000
        or len(samples["room_queue_delay_ms"]) < 100_000
        or len(samples["active_snapshot_interval_ms"]) < 100_000
    ):
        validity.append("SAMPLE_COVERAGE_INSUFFICIENT")
    if _maximum(samples["gameplay_progress_gap_ms"]) > 5_000:
        correctness.append("GAMEPLAY_PROGRESS_STALLED")

    slo = _slo_reasons(run_input["profile"], samples, statistics)
    if facts.operator_aborted:
        status, reasons = "ABORTED", ["OPERATOR_ABORTED"]
    elif validity:
        status, reasons = "INVALID", validity
    elif correctness:
        status, reasons = "FAIL", correctness
    elif slo:
        status, reasons = "FAIL", slo
    else:
        status, reasons = "PASS", []
    reasons.extend(facts.package_reason_codes)
    reasons = _ordered_unique(reasons)
    return {
        "schemaVersion": 1,
        "runId": run_input["runId"],
        "attemptId": run_input["attemptId"],
        "seriesId": run_input["seriesId"],
        "seriesIdentity": run_input["seriesIdentity"],
        "seriesIdentityDigest": run_input["seriesIdentityDigest"],
        "evaluationStatus": status,
        "packageStatus": facts.package_status,
        "reasonCodes": reasons,
        "provenance": run_input["provenance"],
        "statistics": statistics,
    }


def classification_bytes(result: dict[str, Any]) -> bytes:
    errors = validate_document("run-result", result)
    if errors:
        raise ValueError("run result is invalid: " + "; ".join(errors))
    return canonical_json_bytes(result) + b"\n"


def write_classification(path: Path | str, result: dict[str, Any]) -> None:
    classification_bytes(result)
    write_json_atomic(path, result)


def _validate_facts(facts: EvaluationFacts) -> dict[str, tuple[float, ...]]:
    if not isinstance(facts.operator_aborted, bool):
        raise ValueError("operator_aborted must be boolean")
    if facts.package_status not in {"COMPLETE", "INCOMPLETE", "CORRUPT"}:
        raise ValueError("package_status is invalid")
    _reason_subset(facts.package_reason_codes, _PACKAGE_REASONS, "package")
    _reason_subset(facts.validity_reason_codes, _VALIDITY_REASONS, "validity")
    _reason_subset(facts.correctness_reason_codes, _CORRECTNESS_REASONS, "correctness")
    if facts.package_status == "COMPLETE" and facts.package_reason_codes:
        raise ValueError("complete package cannot have package reasons")
    if facts.package_status != "COMPLETE" and not facts.package_reason_codes:
        raise ValueError("non-complete package requires a reason")
    unknown = set(facts.samples) - set(_SAMPLES)
    if unknown:
        raise ValueError(f"unknown samples: {sorted(unknown)!r}")
    samples: dict[str, tuple[float, ...]] = {}
    for name in _SAMPLES:
        raw = facts.samples.get(name, ())
        if len(raw) > 2_000_000:
            raise ValueError(f"{name} exceeds bounded sample count")
        values: list[float] = []
        for value in raw:
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise ValueError(f"{name} contains a non-number")
            number = float(value)
            if not math.isfinite(number) or number < 0:
                raise ValueError(f"{name} contains an invalid number")
            values.append(number)
        samples[name] = tuple(values)
    for value, name, minimum in (
        (facts.expected_metric_scrapes, "expected_metric_scrapes", 1),
        (facts.successful_metric_scrapes, "successful_metric_scrapes", 0),
        (facts.planned_commands, "planned_commands", 0),
        (facts.emitted_commands, "emitted_commands", 0),
        (facts.measurement_duration_ms, "measurement_duration_ms", 0),
        (facts.memory_limit_bytes, "memory_limit_bytes", 1),
        (facts.file_descriptor_limit, "file_descriptor_limit", 1),
    ):
        if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
            raise ValueError(f"{name} must be an integer >= {minimum}")
    if facts.successful_metric_scrapes > facts.expected_metric_scrapes:
        raise ValueError("successful metric scrapes exceed expected")
    if facts.emitted_commands > facts.planned_commands:
        raise ValueError("emitted commands exceed planned commands")
    return samples


def _statistics(
    facts: EvaluationFacts, samples: Mapping[str, tuple[float, ...]]
) -> dict[str, Any]:
    return {
        "sampleCounts": {name: len(samples[name]) for name in _SAMPLES},
        "percentiles": {
            "critical_terminal_latency_p99_ms": _optional_percentile(
                samples["critical_terminal_latency_ms"], 0.99
            ),
            "room_queue_delay_p99_ms": _optional_percentile(samples["room_queue_delay_ms"], 0.99),
            "active_snapshot_interval_p99_ms": _optional_percentile(
                samples["active_snapshot_interval_ms"], 0.99
            ),
            "durable_append_latency_p99_ms": _optional_percentile(
                samples["durable_append_latency_ms"], 0.99
            ),
            "process_cpu_busy_p95_ratio": _optional_percentile(
                samples["process_cpu_busy_ratio"], 0.95
            ),
            "scheduler_slip_p99_ms": _optional_percentile(samples["scheduler_slip_ms"], 0.99),
        },
        "maxima": {name: _optional_maximum(samples[name]) for name in _SAMPLES},
        "ratios": {
            "metrics_coverage": facts.successful_metric_scrapes / facts.expected_metric_scrapes,
            "command_emission_coverage": (
                facts.emitted_commands / facts.planned_commands
                if facts.planned_commands
                else 0.0
            ),
            "memory_usage": _maximum(samples["process_rss_bytes"]) / facts.memory_limit_bytes,
            "file_descriptor_usage": _maximum(samples["process_fd_count"])
            / facts.file_descriptor_limit,
        },
    }


def _slo_reasons(
    profile: Mapping[str, Any],
    samples: Mapping[str, tuple[float, ...]],
    statistics: Mapping[str, Any],
) -> list[str]:
    reasons: list[str] = []
    percentiles = statistics["percentiles"]
    if percentiles["critical_terminal_latency_p99_ms"] is not None and (
        percentiles["critical_terminal_latency_p99_ms"] > 500
        or _maximum(samples["critical_terminal_latency_ms"]) > 2_000
    ):
        reasons.append("CRITICAL_LATENCY_EXCEEDED")
    if percentiles["room_queue_delay_p99_ms"] is not None and (
        percentiles["room_queue_delay_p99_ms"] > 50
        or _maximum(samples["room_queue_delay_ms"]) > 500
    ):
        reasons.append("ROOM_QUEUE_DELAY_EXCEEDED")
    if percentiles["active_snapshot_interval_p99_ms"] is not None and (
        percentiles["active_snapshot_interval_p99_ms"] > 200
        or _maximum(samples["active_snapshot_interval_ms"]) > 500
    ):
        reasons.append("SNAPSHOT_GAP_EXCEEDED")
    if percentiles["durable_append_latency_p99_ms"] is not None and (
        percentiles["durable_append_latency_p99_ms"] > 250
        or _maximum(samples["durable_append_latency_ms"]) > 2_000
    ):
        reasons.append("DURABLE_APPEND_LATENCY_EXCEEDED")
    resource_gate = profile["gateClass"] in {"OFFICIAL_CAPACITY", "FROZEN_REGRESSION_BASELINE"} or (
        profile["gateClass"] == "PRODUCT_RELEASE" and profile["participantCount"] >= 20
    )
    if resource_gate:
        if (percentiles["process_cpu_busy_p95_ratio"] or 0) > 0.85:
            reasons.append("CPU_LIMIT_EXCEEDED")
        if statistics["ratios"]["memory_usage"] > 0.8:
            reasons.append("MEMORY_LIMIT_EXCEEDED")
        if statistics["ratios"]["file_descriptor_usage"] > 0.8:
            reasons.append("FILE_DESCRIPTOR_LIMIT_EXCEEDED")
    if _maximum(samples["outbox_oldest_pending_ms"]) >= 600_000:
        reasons.append("OUTBOX_AGE_EXCEEDED")
    if _maximum(samples["outbox_drain_ms"]) > 120_000:
        reasons.append("OUTBOX_DRAIN_TIMEOUT")
    return reasons


def _percentile(values: Sequence[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def _optional_percentile(values: Sequence[float], fraction: float) -> float | None:
    return _percentile(values, fraction) if values else None


def _maximum(values: Sequence[float]) -> float:
    return max(values, default=0.0)


def _optional_maximum(values: Sequence[float]) -> float | None:
    return max(values) if values else None


def _reason_subset(values: Sequence[str], allowed: frozenset[str], name: str) -> None:
    if len(values) != len(set(values)) or any(value not in allowed for value in values):
        raise ValueError(f"{name} reason codes are invalid")


def _ordered_unique(values: Sequence[str]) -> list[str]:
    return sorted(set(values), key=lambda value: (_REASON_RANK.get(value, 10_000), value))
