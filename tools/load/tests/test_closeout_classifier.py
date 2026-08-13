from __future__ import annotations

import ast
import copy
import json
import tempfile
import unittest
from pathlib import Path

from tools.load.loot_load.evidence.contracts import document_digest, validate_document


try:
    from tools.load.loot_load.closeout.classifier import (
        EvaluationFacts,
        classify,
        classification_bytes,
        write_classification,
    )
except ModuleNotFoundError:
    EvaluationFacts = None
    classify = None
    classification_bytes = None
    write_classification = None


ROOT = Path(__file__).resolve().parents[3]
PROFILES = ROOT / "tools" / "load" / "profiles"
ZERO = "0" * 64
SOURCE = "a" * 40


def run_input(profile_name="release-smoke-10p-v1.json"):
    profile = json.loads((PROFILES / profile_name).read_text())
    profile_digest = document_digest(profile)
    identity = {
        "gameSourceSha": SOURCE,
        "gameArtifactDigest": ZERO,
        "metaSourceSha": SOURCE,
        "metaArtifactDigest": ZERO,
        "harnessSourceSha": SOURCE,
        "harnessArtifactDigest": ZERO,
        "workloadProfileId": profile["profileId"],
        "workloadProfileVersion": profile["version"],
        "workloadProfileDigest": profile_digest,
        "gateVersion": "release-gates-v1",
        "gateDigest": ZERO,
        "classifierVersion": "1.0.0",
        "classifierDigest": ZERO,
        "environmentDigest": ZERO,
        "participantCount": profile["participantCount"],
    }
    provenance = {
        owner: {"sourceSha": SOURCE, "sourceDirty": False, "artifactDigest": ZERO}
        for owner in ("game", "meta", "harness")
    }
    provenance.update(
        {
            "workloadProfileDigest": profile_digest,
            "gateDigest": ZERO,
            "classifierDigest": ZERO,
            "environmentDigest": ZERO,
        }
    )
    return {
        "schemaVersion": 1,
        "runId": "run-0001",
        "attemptId": "attempt-0001",
        "seriesId": "series-0001",
        "seriesIdentity": identity,
        "seriesIdentityDigest": document_digest(identity),
        "provenance": provenance,
        "profile": profile,
    }


def passing_facts(**changes):
    values = {
        "operator_aborted": False,
        "package_status": "COMPLETE",
        "package_reason_codes": (),
        "validity_reason_codes": (),
        "correctness_reason_codes": (),
        "samples": {
            "critical_terminal_latency_ms": (100, 200),
            "room_queue_delay_ms": (10, 20),
            "active_snapshot_interval_ms": (100, 150),
            "durable_append_latency_ms": (100, 200),
            "process_cpu_busy_ratio": (0.4, 0.5),
            "process_rss_bytes": (400, 500),
            "process_fd_count": (10, 20),
            "outbox_oldest_pending_ms": (0,),
            "outbox_drain_ms": (1_000,),
            "gameplay_progress_gap_ms": (100, 1_000),
            "worker_heartbeat_gap_ms": (100, 1_000),
            "scheduler_slip_ms": (10, 20),
        },
        "expected_metric_scrapes": 100,
        "successful_metric_scrapes": 100,
        "planned_commands": 1_000,
        "emitted_commands": 1_000,
        "measurement_duration_ms": 60_000,
        "memory_limit_bytes": 1_000,
        "file_descriptor_limit": 100,
    }
    values.update(changes)
    return EvaluationFacts(**values)


class CloseoutClassifierTests(unittest.TestCase):
    def setUp(self):
        if classify is None:
            self.fail("closeout classifier module is absent")

    def test_precedence_is_aborted_invalid_correctness_slo_pass(self):
        aborted = classify(
            run_input(),
            passing_facts(
                operator_aborted=True,
                validity_reason_codes=("WORKER_CRASHED",),
                correctness_reason_codes=("REMATCH_FAILED",),
                samples={**passing_facts().samples, "critical_terminal_latency_ms": (9_999,)},
            ),
        )
        self.assertEqual("ABORTED", aborted["evaluationStatus"])
        self.assertEqual(["OPERATOR_ABORTED"], aborted["reasonCodes"])

        invalid = classify(
            run_input(),
            passing_facts(
                validity_reason_codes=("WORKER_CRASHED",),
                correctness_reason_codes=("REMATCH_FAILED",),
                samples={**passing_facts().samples, "critical_terminal_latency_ms": (9_999,)},
            ),
        )
        self.assertEqual(("INVALID", ["WORKER_CRASHED"]), self._status(invalid))

        correctness = classify(
            run_input(),
            passing_facts(
                correctness_reason_codes=("REMATCH_FAILED",),
                samples={**passing_facts().samples, "critical_terminal_latency_ms": (9_999,)},
            ),
        )
        self.assertEqual(("FAIL", ["REMATCH_FAILED"]), self._status(correctness))

        slo = classify(
            run_input(),
            passing_facts(
                samples={**passing_facts().samples, "critical_terminal_latency_ms": (501,)},
            ),
        )
        self.assertEqual(("FAIL", ["CRITICAL_LATENCY_EXCEEDED"]), self._status(slo))

        passed = classify(run_input(), passing_facts())
        self.assertEqual(("PASS", []), self._status(passed))

    def test_operator_abort_before_first_command_still_closes_as_aborted(self):
        result = classify(
            run_input(),
            passing_facts(
                operator_aborted=True,
                package_status="INCOMPLETE",
                package_reason_codes=("PACKAGE_FILE_MISSING",),
                planned_commands=0,
                emitted_commands=0,
                measurement_duration_ms=0,
            ),
        )
        self.assertEqual("ABORTED", result["evaluationStatus"])
        self.assertEqual("INCOMPLETE", result["packageStatus"])
        self.assertEqual(
            ["OPERATOR_ABORTED", "PACKAGE_FILE_MISSING"], result["reasonCodes"]
        )

    def test_statistics_are_exact_and_same_input_is_byte_identical(self):
        facts = passing_facts(
            samples={
                **passing_facts().samples,
                "critical_terminal_latency_ms": tuple(range(1, 101)),
            }
        )
        first = classify(run_input(), facts)
        second = classify(copy.deepcopy(run_input()), facts)
        statistics = first["statistics"]
        self.assertEqual(100, statistics["sampleCounts"]["critical_terminal_latency_ms"])
        self.assertEqual(99, statistics["percentiles"]["critical_terminal_latency_p99_ms"])
        self.assertEqual(100, statistics["maxima"]["critical_terminal_latency_ms"])
        self.assertEqual(1.0, statistics["ratios"]["metrics_coverage"])
        self.assertEqual(1.0, statistics["ratios"]["command_emission_coverage"])
        self.assertEqual(classification_bytes(first), classification_bytes(second))
        self.assertEqual([], validate_document("run-result", first))

    def test_official_minimum_samples_are_validity_not_slo_failure(self):
        official = run_input("official-capacity-v1.template.json")
        result = classify(
            official,
            passing_facts(measurement_duration_ms=3_600_000),
        )
        self.assertEqual("INVALID", result["evaluationStatus"])
        self.assertEqual(["SAMPLE_COVERAGE_INSUFFICIENT"], result["reasonCodes"])

    def test_progress_stall_is_correctness_and_package_status_is_independent(self):
        stalled = classify(
            run_input(),
            passing_facts(
                samples={**passing_facts().samples, "gameplay_progress_gap_ms": (5_001,)}
            ),
        )
        self.assertEqual(("FAIL", ["GAMEPLAY_PROGRESS_STALLED"]), self._status(stalled))

        incomplete = classify(
            run_input(),
            passing_facts(
                package_status="INCOMPLETE",
                package_reason_codes=("PACKAGE_FILE_MISSING",),
            ),
        )
        self.assertEqual("PASS", incomplete["evaluationStatus"])
        self.assertEqual("INCOMPLETE", incomplete["packageStatus"])
        self.assertEqual(["PACKAGE_FILE_MISSING"], incomplete["reasonCodes"])
        self.assertEqual([], validate_document("run-result", incomplete))

    def test_incomplete_canonical_workload_is_correctness_failure(self):
        result = classify(
            run_input(),
            passing_facts(
                correctness_reason_codes=("CANONICAL_WORKLOAD_INCOMPLETE",)
            ),
        )

        self.assertEqual(
            ("FAIL", ["CANONICAL_WORKLOAD_INCOMPLETE"]), self._status(result)
        )

    def test_classifier_has_no_network_import_and_result_is_no_clobber(self):
        module = ROOT / "tools" / "load" / "loot_load" / "closeout" / "classifier.py"
        imports = {
            alias.name.split(".")[0]
            for node in ast.walk(ast.parse(module.read_text()))
            if isinstance(node, ast.Import)
            for alias in node.names
        }
        imports.update(
            node.module.split(".")[0]
            for node in ast.walk(ast.parse(module.read_text()))
            if isinstance(node, ast.ImportFrom) and node.module
        )
        self.assertTrue({"socket", "http", "urllib", "requests"}.isdisjoint(imports))
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "evaluation-result.json"
            write_classification(target, classify(run_input(), passing_facts()))
            original = target.read_bytes()
            with self.assertRaises(FileExistsError):
                write_classification(target, classify(run_input(), passing_facts()))
            self.assertEqual(original, target.read_bytes())

    @staticmethod
    def _status(result):
        return result["evaluationStatus"], result["reasonCodes"]


if __name__ == "__main__":
    unittest.main()
