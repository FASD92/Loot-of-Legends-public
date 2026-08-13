"""Bounded, label-free metric snapshot collection for load evidence.

Callers fetch through the bounded private HTTP client and supply response bytes.
"""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass


MAX_RESPONSE_BYTES = 64 * 1024
MAX_METRICS_PER_SNAPSHOT = 64
MAX_PROGRESS_GAP_NS = 5_000_000_000
MIN_COVERAGE = 0.99
_DIGEST = re.compile(r"^[0-9a-f]{64}$")
_METRICS = {
    "game": frozenset(
        {
            "room_queue_delay_ms",
            "room_processing_duration_ms",
            "critical_terminal_latency_ms",
            "active_snapshot_interval_ms",
            "durable_append_latency_ms",
            "outbox_backlog_records",
            "outbox_oldest_pending_ms",
            "outbox_drain_ms",
            "gameplay_progress_total",
            "process_cpu_busy_ratio",
            "process_rss_bytes",
            "process_fd_count",
            "server_invariant_total",
        }
    ),
    "meta": frozenset(
        {
            "settlement_apply_total",
            "settlement_retry_total",
            "settlement_conflict_total",
            "settlement_apply_latency_ms",
            "process_cpu_busy_ratio",
            "process_rss_bytes",
            "process_fd_count",
        }
    ),
    "loadgen": frozenset(
        {
            "worker_heartbeat_age_ms",
            "scheduler_slip_ms",
            "command_planned_total",
            "command_emitted_total",
            "process_cpu_busy_ratio",
            "process_rss_bytes",
            "process_fd_count",
        }
    ),
}


class MetricContractError(ValueError):
    """A snapshot violates the fixed evidence metric contract."""


@dataclass(frozen=True)
class MetricSnapshot:
    source: str
    source_identity_digest: str
    captured_at_unix_ms: int
    metrics: dict[str, float]


@dataclass(frozen=True)
class ScrapeRecord:
    source: str
    source_identity_digest: str | None
    captured_at_unix_ms: int | None
    collected_at_unix_ms: int
    collected_at_monotonic_ns: int
    sample_count: int
    error_code: str | None


@dataclass(frozen=True)
class MetricCoverage:
    ratios: dict[str, float]
    insufficient_metrics: tuple[str, ...]
    reason_codes: tuple[str, ...]


def gameplay_progress_healthy(
    *,
    process_alive: bool,
    gameplay_active: bool,
    now_monotonic_ns: int,
    last_progress_monotonic_ns: int | None,
) -> bool:
    if not process_alive:
        return False
    if not gameplay_active:
        return True
    return (
        last_progress_monotonic_ns is not None
        and now_monotonic_ns >= last_progress_monotonic_ns
        and now_monotonic_ns - last_progress_monotonic_ns <= MAX_PROGRESS_GAP_NS
    )


class MetricsCollector:
    def __init__(self) -> None:
        self._records: list[ScrapeRecord] = []
        self._values: dict[tuple[str, str], list[float]] = {}

    @property
    def records(self) -> tuple[ScrapeRecord, ...]:
        return tuple(self._records)

    def parse(self, raw: bytes) -> MetricSnapshot:
        if not isinstance(raw, bytes) or not raw or len(raw) > MAX_RESPONSE_BYTES:
            raise MetricContractError("metric response size is invalid")
        try:
            value = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise MetricContractError("metric response is not canonical JSON data") from error
        if not isinstance(value, dict) or set(value) != {
            "schemaVersion",
            "source",
            "sourceIdentityDigest",
            "capturedAtUnixMillis",
            "metrics",
        }:
            raise MetricContractError("metric snapshot fields are invalid")
        if value["schemaVersion"] != 1 or value["source"] not in _METRICS:
            raise MetricContractError("metric snapshot version/source is invalid")
        identity = value["sourceIdentityDigest"]
        if not isinstance(identity, str) or _DIGEST.fullmatch(identity) is None:
            raise MetricContractError("metric source identity digest is invalid")
        captured_at = value["capturedAtUnixMillis"]
        if isinstance(captured_at, bool) or not isinstance(captured_at, int) or captured_at < 0:
            raise MetricContractError("metric capture timestamp is invalid")
        entries = value["metrics"]
        if not isinstance(entries, list) or not 1 <= len(entries) <= MAX_METRICS_PER_SNAPSHOT:
            raise MetricContractError("metric entry count is invalid")
        metrics: dict[str, float] = {}
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != {"name", "value"}:
                raise MetricContractError("metric labels or unknown fields are forbidden")
            name = entry["name"]
            number = entry["value"]
            if name not in _METRICS[value["source"]] or name in metrics:
                raise MetricContractError("metric name is unknown or duplicated")
            if (
                isinstance(number, bool)
                or not isinstance(number, (int, float))
                or not math.isfinite(number)
                or number < 0
            ):
                raise MetricContractError("metric value must be finite and non-negative")
            metrics[name] = float(number)
        return MetricSnapshot(value["source"], identity, captured_at, metrics)

    def record(
        self,
        raw: bytes,
        *,
        collected_at_unix_ms: int,
        collected_at_monotonic_ns: int,
        source_hint: str | None = None,
        expected_identity_digest: str | None = None,
    ) -> ScrapeRecord:
        _timestamp(collected_at_unix_ms, "collected_at_unix_ms")
        _timestamp(collected_at_monotonic_ns, "collected_at_monotonic_ns")
        expected_identity = (
            _normalize_digest(expected_identity_digest)
            if expected_identity_digest is not None
            else None
        )
        try:
            snapshot = self.parse(raw)
        except MetricContractError:
            source = source_hint if source_hint in _METRICS else "unknown"
            record = ScrapeRecord(
                source,
                None,
                None,
                collected_at_unix_ms,
                collected_at_monotonic_ns,
                0,
                "SCRAPE_RESPONSE_INVALID",
            )
            self._records.append(record)
            return record
        if (
            (source_hint in _METRICS and snapshot.source != source_hint)
            or (
                expected_identity is not None
                and snapshot.source_identity_digest != expected_identity
            )
        ):
            record = ScrapeRecord(
                snapshot.source,
                snapshot.source_identity_digest,
                snapshot.captured_at_unix_ms,
                collected_at_unix_ms,
                collected_at_monotonic_ns,
                0,
                "PROVENANCE_MISMATCH",
            )
            self._records.append(record)
            return record
        for name, number in snapshot.metrics.items():
            self._values.setdefault((snapshot.source, name), []).append(number)
        record = ScrapeRecord(
            snapshot.source,
            snapshot.source_identity_digest,
            snapshot.captured_at_unix_ms,
            collected_at_unix_ms,
            collected_at_monotonic_ns,
            len(snapshot.metrics),
            None,
        )
        self._records.append(record)
        return record

    def values(self, source: str, metric: str) -> tuple[float, ...]:
        return tuple(self._values.get((source, metric), ()))

    def coverage(
        self,
        source: str,
        *,
        required_metrics: tuple[str, ...],
        expected_scrapes: int,
    ) -> MetricCoverage:
        if source not in _METRICS or expected_scrapes <= 0:
            raise ValueError("coverage source/expected_scrapes is invalid")
        if any(metric not in _METRICS[source] for metric in required_metrics):
            raise ValueError("coverage contains an unknown metric")
        ratios = {
            metric: min(1.0, len(self._values.get((source, metric), ())) / expected_scrapes)
            for metric in required_metrics
        }
        insufficient = tuple(sorted(metric for metric, ratio in ratios.items() if ratio < MIN_COVERAGE))
        reasons = ("METRICS_COVERAGE_INSUFFICIENT",) if insufficient else ()
        return MetricCoverage(ratios, insufficient, reasons)


def _timestamp(value: object, field: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{field} must be a non-negative integer")


def _normalize_digest(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("expected identity digest must be a string")
    digest = value.removeprefix("sha256:")
    if _DIGEST.fullmatch(digest) is None or value not in {digest, "sha256:" + digest}:
        raise ValueError("expected identity digest format is invalid")
    return digest
