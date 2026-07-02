import json
import tempfile
import unittest
from pathlib import Path

from scripts.release1.capacity_report import (
    evaluate_capacity_run,
    load_json,
    sha256_file,
    validate_schema,
)
from scripts.release1.summary_orchestrator import build_summary, main
from scripts.release1.test_capacity_report import make_run_config


REPO_ROOT = Path(__file__).resolve().parents[2]
GATE_CONFIG_PATH = REPO_ROOT / "scripts" / "release1" / "gate_config.json"
SUMMARY_SCHEMA_PATH = (
    REPO_ROOT / "scripts" / "release1" / "schemas" / "summary.schema.json"
)


class SummaryOrchestratorTests(unittest.TestCase):
    def test_completed_clean_summary_validates_and_evaluates_to_pass(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = build_summary(
            run_id="20260701-010203-clean-deadbee",
            completed_official_window=True,
        )

        validate_schema(summary, load_json(SUMMARY_SCHEMA_PATH))
        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("PASS", result["result_status"])

    def test_failed_hard_gate_summary_evaluates_to_fail(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = build_summary(
            run_id="20260701-010203-clean-deadbee",
            completed_official_window=True,
            failed_hard_gate_categories=["Primary latency", "Workload"],
        )

        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("FAIL", result["result_status"])
        self.assertEqual(
            ["Primary latency", "Workload"],
            result["failed_hard_gate_categories"],
        )

    def test_invalid_artifact_summary_evaluates_to_invalid(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = build_summary(
            run_id="20260701-010203-clean-deadbee",
            completed_official_window=True,
            invalid_category="Artifact",
            invalid_reason="missing Prometheus metrics",
        )

        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Artifact", result["invalid_category"])

    def test_gate_evaluation_fail_adds_failed_hard_gate(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = build_summary(
            run_id="20260701-010203-clean-deadbee",
            completed_official_window=True,
            gate_evaluations=[
                {
                    "result_status": "FAIL",
                    "failed_hard_gate_categories": ["Primary latency"],
                    "reason": "primary_latency_p99_exceeded",
                }
            ],
        )

        validate_schema(summary, load_json(SUMMARY_SCHEMA_PATH))
        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("FAIL", result["result_status"])
        self.assertEqual(["Primary latency"], result["failed_hard_gate_categories"])

    def test_gate_evaluation_invalid_sets_summary_validity(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = build_summary(
            run_id="20260701-010203-clean-deadbee",
            completed_official_window=True,
            gate_evaluations=[
                {
                    "result_status": "INVALID",
                    "invalid_category": "Workload",
                    "reason": "insufficient_primary_latency_samples",
                }
            ],
        )

        validate_schema(summary, load_json(SUMMARY_SCHEMA_PATH))
        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Workload", result["invalid_category"])
        self.assertEqual("insufficient_primary_latency_samples", result["reason"])

    def test_cli_writes_schema_compatible_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "summary.json"

            status = main(
                [
                    "--run-id",
                    "20260701-010203-clean-deadbee",
                    "--completed-official-window",
                    "--failed-hard-gate",
                    "Primary latency",
                    "--regression-comparison-json",
                    json.dumps(
                        {
                            "name": "previous",
                            "gate_config_sha256": "0" * 64,
                        }
                    ),
                    "--output",
                    str(output),
                ]
            )

            self.assertEqual(0, status)
            summary = load_json(output)

        validate_schema(summary, load_json(SUMMARY_SCHEMA_PATH))
        self.assertEqual(
            ["Primary latency"],
            summary["hard_gates"]["failed_categories"],
        )
        self.assertEqual("previous", summary["regression_comparisons"][0]["name"])

    def test_cli_accepts_gate_evaluation_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "summary.json"

            status = main(
                [
                    "--run-id",
                    "20260701-010203-clean-deadbee",
                    "--completed-official-window",
                    "--gate-evaluation-json",
                    json.dumps(
                        {
                            "result_status": "FAIL",
                            "failed_hard_gate_categories": ["Primary latency"],
                            "reason": "primary_latency_p99_exceeded",
                        }
                    ),
                    "--output",
                    str(output),
                ]
            )

            self.assertEqual(0, status)
            summary = load_json(output)

        validate_schema(summary, load_json(SUMMARY_SCHEMA_PATH))
        self.assertEqual(
            ["Primary latency"],
            summary["hard_gates"]["failed_categories"],
        )

    def test_cli_accepts_gate_evaluation_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "summary.json"
            gate_evaluation = root / "accepted-delivery-evaluation.json"
            gate_evaluation.write_text(
                json.dumps(
                    {
                        "result_status": "FAIL",
                        "failed_hard_gate_categories": ["Correctness"],
                        "reason": "server_accepted_delivery_ratio_below_min",
                    }
                ),
                encoding="utf-8",
            )

            status = main(
                [
                    "--run-id",
                    "20260701-010203-clean-deadbee",
                    "--completed-official-window",
                    "--gate-evaluation-file",
                    str(gate_evaluation),
                    "--output",
                    str(output),
                ]
            )

            self.assertEqual(0, status)
            summary = load_json(output)

        validate_schema(summary, load_json(SUMMARY_SCHEMA_PATH))
        self.assertEqual(
            ["Correctness"],
            summary["hard_gates"]["failed_categories"],
        )


if __name__ == "__main__":
    unittest.main()
