from __future__ import annotations

import json
import unittest


try:
    from tools.load.loot_load.evidence.metrics import (
        MetricContractError,
        MetricsCollector,
        gameplay_progress_healthy,
    )
except ModuleNotFoundError:
    MetricContractError = None
    MetricsCollector = None
    gameplay_progress_healthy = None


SOURCE_IDENTITY = "a" * 64


def snapshot(source, metrics, captured_at=1_000, identity=SOURCE_IDENTITY):
    return json.dumps(
        {
            "schemaVersion": 1,
            "source": source,
            "sourceIdentityDigest": identity,
            "capturedAtUnixMillis": captured_at,
            "metrics": [{"name": name, "value": value} for name, value in metrics],
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode()


class MetricsCollectionTests(unittest.TestCase):
    def setUp(self):
        if MetricsCollector is None:
            self.fail("metrics collector module is absent")

    def test_alive_process_without_active_gameplay_progress_is_unhealthy(self):
        second = 1_000_000_000
        self.assertTrue(
            gameplay_progress_healthy(
                process_alive=True,
                gameplay_active=True,
                now_monotonic_ns=6 * second,
                last_progress_monotonic_ns=second,
            )
        )
        self.assertFalse(
            gameplay_progress_healthy(
                process_alive=True,
                gameplay_active=True,
                now_monotonic_ns=6 * second + 1,
                last_progress_monotonic_ns=second,
            )
        )
        self.assertFalse(
            gameplay_progress_healthy(
                process_alive=True,
                gameplay_active=True,
                now_monotonic_ns=second,
                last_progress_monotonic_ns=None,
            )
        )
        self.assertTrue(
            gameplay_progress_healthy(
                process_alive=True,
                gameplay_active=False,
                now_monotonic_ns=10 * second,
                last_progress_monotonic_ns=None,
            )
        )

    def test_collection_records_source_timestamps_values_and_scrape_errors(self):
        collector = MetricsCollector()
        record = collector.record(
            snapshot(
                "game",
                (
                    ("gameplay_progress_total", 7),
                    ("room_queue_delay_ms", 4.5),
                ),
            ),
            collected_at_unix_ms=1_010,
            collected_at_monotonic_ns=9_000,
        )
        self.assertIsNone(record.error_code)
        self.assertEqual("game", record.source)
        self.assertEqual(SOURCE_IDENTITY, record.source_identity_digest)
        self.assertEqual(1_010, record.collected_at_unix_ms)
        self.assertEqual(9_000, record.collected_at_monotonic_ns)
        self.assertEqual((4.5,), collector.values("game", "room_queue_delay_ms"))

        failed = collector.record(
            b"not-json",
            source_hint="meta",
            collected_at_unix_ms=2_010,
            collected_at_monotonic_ns=10_000,
        )
        self.assertEqual("SCRAPE_RESPONSE_INVALID", failed.error_code)
        self.assertEqual("meta", failed.source)
        self.assertEqual((), collector.values("meta", "settlement_apply_total"))

    def test_missing_metric_is_coverage_failure_input_not_silent_zero(self):
        collector = MetricsCollector()
        collector.record(
            snapshot(
                "game",
                (
                    ("gameplay_progress_total", 1),
                    ("room_queue_delay_ms", 2),
                ),
            ),
            collected_at_unix_ms=1_000,
            collected_at_monotonic_ns=1_000,
        )
        collector.record(
            snapshot("game", (("gameplay_progress_total", 2),), captured_at=2_000),
            collected_at_unix_ms=2_000,
            collected_at_monotonic_ns=2_000,
        )

        coverage = collector.coverage(
            "game",
            required_metrics=("gameplay_progress_total", "room_queue_delay_ms"),
            expected_scrapes=2,
        )
        self.assertEqual(1.0, coverage.ratios["gameplay_progress_total"])
        self.assertEqual(0.5, coverage.ratios["room_queue_delay_ms"])
        self.assertEqual(("room_queue_delay_ms",), coverage.insufficient_metrics)
        self.assertEqual(("METRICS_COVERAGE_INSUFFICIENT",), coverage.reason_codes)
        self.assertEqual((2.0,), collector.values("game", "room_queue_delay_ms"))

    def test_provenance_mismatch_and_source_swap_admit_no_metric_values(self):
        collector = MetricsCollector()
        mismatch = collector.record(
            snapshot("game", (("room_queue_delay_ms", 7),), identity="b" * 64),
            collected_at_unix_ms=1_000,
            collected_at_monotonic_ns=1_000,
            source_hint="game",
            expected_identity_digest="sha256:" + SOURCE_IDENTITY,
        )
        swapped = collector.record(
            snapshot("meta", (("settlement_apply_total", 3),)),
            collected_at_unix_ms=2_000,
            collected_at_monotonic_ns=2_000,
            source_hint="game",
            expected_identity_digest=SOURCE_IDENTITY,
        )

        self.assertEqual("PROVENANCE_MISMATCH", mismatch.error_code)
        self.assertEqual("PROVENANCE_MISMATCH", swapped.error_code)
        self.assertEqual(0, mismatch.sample_count)
        self.assertEqual(0, swapped.sample_count)
        self.assertEqual((), collector.values("game", "room_queue_delay_ms"))
        self.assertEqual((), collector.values("meta", "settlement_apply_total"))

        matched = MetricsCollector()
        accepted = matched.record(
            snapshot("game", (("room_queue_delay_ms", 7),)),
            collected_at_unix_ms=3_000,
            collected_at_monotonic_ns=3_000,
            source_hint="game",
            expected_identity_digest="sha256:" + SOURCE_IDENTITY,
        )
        accepted_raw = matched.record(
            snapshot("game", (("room_queue_delay_ms", 8),)),
            collected_at_unix_ms=4_000,
            collected_at_monotonic_ns=4_000,
            source_hint="game",
            expected_identity_digest=SOURCE_IDENTITY,
        )
        self.assertIsNone(accepted.error_code)
        self.assertIsNone(accepted_raw.error_code)
        self.assertEqual((7.0, 8.0), matched.values("game", "room_queue_delay_ms"))
        with self.assertRaises(ValueError):
            matched.record(
                snapshot("game", (("room_queue_delay_ms", 9),)),
                collected_at_unix_ms=5_000,
                collected_at_monotonic_ns=5_000,
                expected_identity_digest="sha256:sha256:" + SOURCE_IDENTITY,
            )

    def test_unknown_metrics_labels_and_identity_are_rejected(self):
        collector = MetricsCollector()
        labelled = json.loads(snapshot("game", (("room_queue_delay_ms", 1),)))
        labelled["metrics"][0]["labels"] = {"roomId": "7"}
        with self.assertRaises(MetricContractError):
            collector.parse(json.dumps(labelled).encode())

        with self.assertRaises(MetricContractError):
            collector.parse(snapshot("game", (("account_id", 1),)))

        invalid_identity = json.loads(snapshot("game", (("room_queue_delay_ms", 1),)))
        invalid_identity["sourceIdentityDigest"] = "not-a-digest"
        with self.assertRaises(MetricContractError):
            collector.parse(json.dumps(invalid_identity).encode())


if __name__ == "__main__":
    unittest.main()
