import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from scripts.release1.capacity_report import (
    ValidationError,
    conservative_bucket_percentile_ms,
    evaluate_capacity_run,
    evaluate_primary_latency_metric,
    evaluate_primary_latency_metrics,
    evaluate_runner_delivery,
    evaluate_server_accepted_delivery,
    extract_latency_bucket_metric,
    load_json,
    main,
    sha256_file,
    summarize_latency_bucket_metric,
    validate_schema,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
GATE_CONFIG_PATH = REPO_ROOT / "scripts" / "release1" / "gate_config.json"
SUMMARY_SCHEMA_PATH = (
    REPO_ROOT / "scripts" / "release1" / "schemas" / "summary.schema.json"
)
RUN_CONFIG_SCHEMA_PATH = (
    REPO_ROOT / "scripts" / "release1" / "schemas" / "run_config.schema.json"
)


def make_summary():
    return {
        "schema_version": "release1.capacity.summary.v1",
        "run_id": "20260630-120000-clean-deadbee",
        "completed_official_window": True,
        "operator_aborted": False,
        "validity": {
            "valid": True,
            "invalid_category": "",
            "reason": "",
        },
        "hard_gates": {
            "all_passed": True,
            "failed_categories": [],
        },
        "regression_comparisons": [],
    }


def make_run_config(gate_config_sha256):
    execution = {
        "executable_path": "/opt/loot-of-legends/app/lol_server",
        "argv": ["lol_server", "--config", "release1"],
        "working_directory": "/opt/loot-of-legends/app",
        "binary_sha256": "a" * 64,
        "environment": {"LOL_ENV": "release1"},
        "ulimit": {"nofile": 1048576},
    }
    return {
        "schema_version": "release1.capacity.run_config.v1",
        "run_id": "20260630-120000-clean-deadbee",
        "gate_config_sha256": gate_config_sha256,
        "execution": {
            "server": execution,
            "stress_runner": {
                **execution,
                "executable_path": "scripts/release1/stress_runner.py",
                "argv": ["python3", "scripts/release1/stress_runner.py"],
            },
        },
    }


def latency_metric_text(metric_name, le_counts, total_count):
    lines = [
        f'{metric_name}_bucket{{le="{upper_bound_ms}"}} {count}'
        for upper_bound_ms, count in le_counts
    ]
    lines.append(f"{metric_name}_count {total_count}")
    return "\n".join(lines)


def runner_delivery_result(
    *,
    move_target=100,
    move_sent=100,
    attack_target=100,
    attack_sent=100,
    loot_target=100,
    loot_sent=100,
    loot_accept_target=None,
):
    result = {
        "rudp_move_target_sent": move_target,
        "rudp_move_sent": move_sent,
        "rudp_attack_target_sent": attack_target,
        "rudp_attack_sent": attack_sent,
        "rudp_click_loot_target_sent": loot_target,
        "rudp_click_loot_sent": loot_sent,
    }
    result["rudp_click_loot_server_accept_target_sent"] = (
        loot_target if loot_accept_target is None else loot_accept_target
    )
    return result


def accepted_delivery_metrics_text(*, move=100, attack=100, loot=100):
    return "\n".join(
        [
            f"lol_rudp_move_accepted_total {move}",
            f"lol_rudp_attack_accepted_total {attack}",
            f"lol_rudp_loot_claim_accepted_total {loot}",
        ]
    )


class CapacityReportTests(unittest.TestCase):
    def test_valid_summary_and_matching_gate_config_produce_pass(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        result = evaluate_capacity_run(
            make_summary(),
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("PASS", result["result_status"])

    def test_hard_gate_failure_produces_fail(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = make_summary()
        summary["hard_gates"] = {
            "all_passed": False,
            "failed_categories": ["Primary latency"],
        }

        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("FAIL", result["result_status"])
        self.assertEqual(["Primary latency"], result["failed_hard_gate_categories"])

    def test_invalid_run_evidence_produces_invalid(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = make_summary()
        summary["validity"] = {
            "valid": False,
            "invalid_category": "Artifact",
            "reason": "missing summary checksum",
        }

        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Artifact", result["invalid_category"])

    def test_incomplete_run_without_hard_gate_failure_produces_aborted(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = make_summary()
        summary["completed_official_window"] = False

        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        self.assertEqual("ABORTED", result["result_status"])

    def test_gate_config_checksum_mismatch_produces_invalid(self):
        result = evaluate_capacity_run(
            make_summary(),
            make_run_config("0" * 64),
            "1" * 64,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Artifact", result["invalid_category"])

    def test_summary_schema_rejects_unknown_top_level_field(self):
        schema = load_json(SUMMARY_SCHEMA_PATH)
        summary = make_summary()
        summary["unexpected"] = True

        with self.assertRaisesRegex(ValidationError, "unexpected field"):
            validate_schema(summary, schema)

    def test_run_config_schema_rejects_unapproved_environment_key(self):
        schema = load_json(RUN_CONFIG_SCHEMA_PATH)
        run_config = make_run_config("a" * 64)
        run_config["execution"]["server"]["environment"]["SECRET_TOKEN"] = "raw"

        with self.assertRaisesRegex(ValidationError, "unexpected field SECRET_TOKEN"):
            validate_schema(run_config, schema)

    def test_regression_comparison_is_skipped_when_gate_config_differs(self):
        gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
        summary = make_summary()
        summary["regression_comparisons"] = [
            {
                "name": "previous",
                "gate_config_sha256": "0" * 64,
            },
            {
                "name": "frozen",
                "gate_config_sha256": gate_config_sha256,
            },
        ]

        result = evaluate_capacity_run(
            summary,
            make_run_config(gate_config_sha256),
            gate_config_sha256,
        )

        comparisons = result["regression_comparisons"]
        self.assertEqual(
            "gate_config_checksum_mismatch",
            comparisons[0]["skipped_reason"],
        )
        self.assertEqual("", comparisons[1]["skipped_reason"])

    def test_latency_bucket_percentile_uses_conservative_upper_bound(self):
        buckets = [(1.0, 10), (2.0, 50), (5.0, 95), (10.0, 100)]

        self.assertEqual(
            2.0,
            conservative_bucket_percentile_ms(buckets, percentile=50.0, total_count=100),
        )
        self.assertEqual(
            10.0,
            conservative_bucket_percentile_ms(buckets, percentile=99.0, total_count=100),
        )

    def test_latency_bucket_percentile_rejects_non_monotonic_counts(self):
        buckets = [(1.0, 10), (2.0, 9)]

        with self.assertRaisesRegex(ValidationError, "non-monotonic"):
            conservative_bucket_percentile_ms(
                buckets,
                percentile=95.0,
                total_count=10,
            )

    def test_latency_bucket_percentile_returns_none_when_target_is_above_recorded_buckets(
        self,
    ):
        buckets = [(1.0, 10), (2.0, 20)]

        self.assertIsNone(
            conservative_bucket_percentile_ms(
                buckets,
                percentile=99.0,
                total_count=100,
            )
        )

    def test_latency_bucket_percentile_returns_none_without_samples(self):
        self.assertIsNone(
            conservative_bucket_percentile_ms(
                [(1.0, 0), (2.0, 0)],
                percentile=99.0,
                total_count=0,
            )
        )

    def test_extract_latency_bucket_metric_reads_prometheus_textfile(self):
        metrics_text = "\n".join(
            [
                "# HELP lol_server_runtime_tick_total Runtime ticks.",
                "lol_server_runtime_tick_total 10",
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="1"} 2',
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="2"} 3',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 3",
                "lol_rudp_attack_receive_to_apply_latency_ms_sum 1.2",
            ]
        )

        buckets, total_count = extract_latency_bucket_metric(
            metrics_text,
            "lol_rudp_attack_receive_to_apply_latency_ms",
        )

        self.assertEqual([(1.0, 2), (2.0, 3)], buckets)
        self.assertEqual(3, total_count)

    def test_extract_latency_bucket_metric_feeds_conservative_percentile(self):
        metrics_text = "\n".join(
            [
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="1"} 10',
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="20"} 95',
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="50"} 100',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 100",
            ]
        )
        buckets, total_count = extract_latency_bucket_metric(
            metrics_text,
            "lol_rudp_attack_receive_to_apply_latency_ms",
        )

        self.assertEqual(
            50.0,
            conservative_bucket_percentile_ms(
                buckets,
                percentile=99.0,
                total_count=total_count,
            ),
        )

    def test_extract_latency_bucket_metric_requires_count(self):
        metrics_text = 'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="1"} 1'

        with self.assertRaisesRegex(ValidationError, "missing count"):
            extract_latency_bucket_metric(
                metrics_text,
                "lol_rudp_attack_receive_to_apply_latency_ms",
            )

    def test_extract_latency_bucket_metric_rejects_non_integer_counts(self):
        metrics_text = "\n".join(
            [
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="1"} 1.5',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 2",
            ]
        )

        with self.assertRaisesRegex(ValidationError, "integer"):
            extract_latency_bucket_metric(
                metrics_text,
                "lol_rudp_attack_receive_to_apply_latency_ms",
            )

    def test_summarize_latency_bucket_metric_reports_count_p95_p99(self):
        metrics_text = "\n".join(
            [
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="1"} 10',
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="20"} 95',
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="50"} 100',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 100",
            ]
        )

        summary = summarize_latency_bucket_metric(
            metrics_text,
            "lol_rudp_attack_receive_to_apply_latency_ms",
        )

        self.assertEqual(
            {
                "sample_count": 100,
                "p95_ms": 20.0,
                "p99_ms": 50.0,
            },
            summary,
        )

    def test_summarize_latency_bucket_metric_reports_none_when_p99_uncovered(self):
        metrics_text = "\n".join(
            [
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="1"} 10',
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="2"} 20',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 100",
            ]
        )

        summary = summarize_latency_bucket_metric(
            metrics_text,
            "lol_rudp_attack_receive_to_apply_latency_ms",
        )

        self.assertEqual(100, summary["sample_count"])
        self.assertIsNone(summary["p99_ms"])

    def test_primary_latency_metric_passes_when_p99_is_within_slo(self):
        metrics_text = "\n".join(
            [
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="20"} 95',
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="50"} 100',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 100",
            ]
        )

        result = evaluate_primary_latency_metric(
            metrics_text,
            "lol_rudp_attack_receive_to_apply_latency_ms",
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("PASS", result["result_status"])
        self.assertEqual(50.0, result["p99_ms"])

    def test_primary_latency_metric_fails_when_p99_exceeds_slo(self):
        metrics_text = "\n".join(
            [
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="50"} 98',
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="75"} 100',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 100",
            ]
        )

        result = evaluate_primary_latency_metric(
            metrics_text,
            "lol_rudp_attack_receive_to_apply_latency_ms",
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("FAIL", result["result_status"])
        self.assertEqual(["Primary latency"], result["failed_hard_gate_categories"])
        self.assertEqual("primary_latency_p99_exceeded", result["reason"])

    def test_primary_latency_metric_is_invalid_workload_when_samples_are_insufficient(
        self,
    ):
        metrics_text = "\n".join(
            [
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="20"} 99',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 99",
            ]
        )

        result = evaluate_primary_latency_metric(
            metrics_text,
            "lol_rudp_attack_receive_to_apply_latency_ms",
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Workload", result["invalid_category"])
        self.assertEqual("insufficient_primary_latency_samples", result["reason"])

    def test_primary_latency_metric_is_invalid_artifact_when_metric_is_missing(self):
        result = evaluate_primary_latency_metric(
            "lol_server_runtime_tick_total 10",
            "lol_rudp_attack_receive_to_apply_latency_ms",
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Artifact", result["invalid_category"])

    def test_primary_latency_metric_is_invalid_artifact_when_p99_is_uncovered(self):
        metrics_text = "\n".join(
            [
                'lol_rudp_attack_receive_to_apply_latency_ms_bucket{le="20"} 20',
                "lol_rudp_attack_receive_to_apply_latency_ms_count 100",
            ]
        )

        result = evaluate_primary_latency_metric(
            metrics_text,
            "lol_rudp_attack_receive_to_apply_latency_ms",
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Artifact", result["invalid_category"])
        self.assertEqual("primary_latency_p99_uncovered", result["reason"])

    def test_primary_latency_metrics_pass_when_all_ops_pass(self):
        op_metrics = {
            "Move": "lol_rudp_move_receive_to_apply_latency_ms",
            "Attack": "lol_rudp_attack_receive_to_apply_latency_ms",
        }
        metrics_text = "\n".join(
            [
                latency_metric_text(op_metrics["Move"], [(20, 95), (50, 100)], 100),
                latency_metric_text(op_metrics["Attack"], [(20, 95), (50, 100)], 100),
            ]
        )

        result = evaluate_primary_latency_metrics(
            metrics_text,
            op_metrics,
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("PASS", result["result_status"])
        self.assertEqual(["Attack", "Move"], sorted(result["op_results"].keys()))

    def test_primary_latency_metrics_fail_when_any_valid_op_exceeds_slo(self):
        op_metrics = {
            "Move": "lol_rudp_move_receive_to_apply_latency_ms",
            "Attack": "lol_rudp_attack_receive_to_apply_latency_ms",
        }
        metrics_text = "\n".join(
            [
                latency_metric_text(op_metrics["Move"], [(20, 95), (50, 100)], 100),
                latency_metric_text(op_metrics["Attack"], [(50, 98), (75, 100)], 100),
            ]
        )

        result = evaluate_primary_latency_metrics(
            metrics_text,
            op_metrics,
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("FAIL", result["result_status"])
        self.assertEqual(["Primary latency"], result["failed_hard_gate_categories"])
        self.assertEqual(["Attack"], result["failed_ops"])

    def test_primary_latency_metrics_are_invalid_artifact_when_any_required_op_metric_is_missing(
        self,
    ):
        op_metrics = {
            "Move": "lol_rudp_move_receive_to_apply_latency_ms",
            "Attack": "lol_rudp_attack_receive_to_apply_latency_ms",
        }
        metrics_text = latency_metric_text(
            op_metrics["Move"],
            [(20, 95), (50, 100)],
            100,
        )

        result = evaluate_primary_latency_metrics(
            metrics_text,
            op_metrics,
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Artifact", result["invalid_category"])
        self.assertEqual(["Attack"], result["invalid_ops"])

    def test_primary_latency_metrics_are_invalid_workload_when_any_required_op_has_too_few_samples(
        self,
    ):
        op_metrics = {
            "Move": "lol_rudp_move_receive_to_apply_latency_ms",
            "Attack": "lol_rudp_attack_receive_to_apply_latency_ms",
        }
        metrics_text = "\n".join(
            [
                latency_metric_text(op_metrics["Move"], [(20, 95), (50, 100)], 100),
                latency_metric_text(op_metrics["Attack"], [(20, 99)], 99),
            ]
        )

        result = evaluate_primary_latency_metrics(
            metrics_text,
            op_metrics,
            min_samples=100,
            p99_slo_ms=50,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Workload", result["invalid_category"])
        self.assertEqual(["Attack"], result["invalid_ops"])

    def test_runner_delivery_passes_when_actual_meets_min_ratio(self):
        result = evaluate_runner_delivery(
            runner_delivery_result(move_sent=99),
            min_ratio=0.99,
        )

        self.assertEqual("PASS", result["result_status"])
        self.assertEqual(0.99, result["op_results"]["Move"]["delivery_ratio"])

    def test_runner_delivery_fails_when_actual_is_below_min_ratio(self):
        result = evaluate_runner_delivery(
            runner_delivery_result(attack_sent=98),
            min_ratio=0.99,
        )

        self.assertEqual("FAIL", result["result_status"])
        self.assertEqual(["Correctness"], result["failed_hard_gate_categories"])
        self.assertEqual("runner_delivery_ratio_below_min", result["reason"])
        self.assertEqual(["Attack"], result["failed_ops"])

    def test_runner_delivery_is_invalid_artifact_when_counter_is_missing(self):
        runner_result = runner_delivery_result()
        del runner_result["rudp_click_loot_sent"]

        result = evaluate_runner_delivery(runner_result, min_ratio=0.99)

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Artifact", result["invalid_category"])
        self.assertEqual("runner_delivery_counter_missing", result["reason"])
        self.assertEqual(["Loot claim"], result["invalid_ops"])

    def test_runner_delivery_is_invalid_workload_when_target_is_zero(self):
        result = evaluate_runner_delivery(
            runner_delivery_result(loot_target=0, loot_sent=0),
            min_ratio=0.99,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Workload", result["invalid_category"])
        self.assertEqual("runner_delivery_target_missing", result["reason"])
        self.assertEqual(["Loot claim"], result["invalid_ops"])

    def test_cli_evaluates_runner_delivery_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            runner_path = Path(tmp) / "runner.json"
            runner_path.write_text(
                json.dumps(runner_delivery_result(move_sent=99)),
                encoding="utf-8",
            )
            output = io.StringIO()

            with redirect_stdout(output):
                status = main(
                    [
                        "--runner-result",
                        str(runner_path),
                        "--runner-delivery-min-ratio",
                        "0.99",
                    ]
                )

        self.assertEqual(0, status)
        result = json.loads(output.getvalue())
        self.assertEqual("PASS", result["result_status"])
        self.assertEqual(["Attack", "Loot claim", "Move"], sorted(result["op_results"]))

    def test_server_accepted_delivery_passes_when_metrics_meet_targets(self):
        result = evaluate_server_accepted_delivery(
            runner_delivery_result(move_target=2, attack_target=8, loot_target=2),
            accepted_delivery_metrics_text(move=2, attack=8, loot=2),
            min_ratio=0.99,
        )

        self.assertEqual("PASS", result["result_status"])
        self.assertEqual(1.0, result["op_results"]["Loot claim"]["accepted_ratio"])

    def test_server_accepted_delivery_uses_loot_accept_target_when_present(self):
        runner = runner_delivery_result(
            move_target=2,
            move_sent=2,
            attack_target=8,
            attack_sent=8,
            loot_target=10,
            loot_sent=10,
            loot_accept_target=1,
        )

        runner_result = evaluate_runner_delivery(runner, min_ratio=0.99)
        accepted_result = evaluate_server_accepted_delivery(
            runner,
            accepted_delivery_metrics_text(move=2, attack=8, loot=1),
            min_ratio=0.99,
        )

        self.assertEqual("PASS", runner_result["result_status"])
        self.assertEqual(10, runner_result["op_results"]["Loot claim"]["target_sent"])
        self.assertEqual("PASS", accepted_result["result_status"])
        self.assertEqual(1, accepted_result["op_results"]["Loot claim"]["target_sent"])

    def test_server_accepted_delivery_fails_when_metric_is_below_target(self):
        result = evaluate_server_accepted_delivery(
            runner_delivery_result(move_target=2, attack_target=8, loot_target=2),
            accepted_delivery_metrics_text(move=2, attack=8, loot=1),
            min_ratio=0.99,
        )

        self.assertEqual("FAIL", result["result_status"])
        self.assertEqual(["Correctness"], result["failed_hard_gate_categories"])
        self.assertEqual("server_accepted_delivery_ratio_below_min", result["reason"])
        self.assertEqual(["Loot claim"], result["failed_ops"])

    def test_server_accepted_delivery_is_invalid_artifact_when_metric_is_missing(self):
        result = evaluate_server_accepted_delivery(
            runner_delivery_result(move_target=2, attack_target=8, loot_target=2),
            "\n".join(
                [
                    "lol_rudp_move_accepted_total 2",
                    "lol_rudp_attack_accepted_total 8",
                ]
            ),
            min_ratio=0.99,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Artifact", result["invalid_category"])
        self.assertEqual("server_accepted_delivery_metric_missing", result["reason"])
        self.assertEqual(["Loot claim"], result["invalid_ops"])

    def test_server_accepted_delivery_is_invalid_workload_when_target_is_zero(self):
        result = evaluate_server_accepted_delivery(
            runner_delivery_result(move_target=2, attack_target=8, loot_target=0),
            accepted_delivery_metrics_text(move=2, attack=8, loot=0),
            min_ratio=0.99,
        )

        self.assertEqual("INVALID", result["result_status"])
        self.assertEqual("Workload", result["invalid_category"])
        self.assertEqual("server_accepted_delivery_target_missing", result["reason"])
        self.assertEqual(["Loot claim"], result["invalid_ops"])

    def test_cli_evaluates_server_accepted_delivery_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            runner_path = Path(tmp) / "runner.json"
            metrics_path = Path(tmp) / "lol_game.prom"
            runner_path.write_text(
                json.dumps(
                    runner_delivery_result(
                        move_target=2,
                        move_sent=2,
                        attack_target=8,
                        attack_sent=8,
                        loot_target=2,
                        loot_sent=2,
                    )
                ),
                encoding="utf-8",
            )
            metrics_path.write_text(
                accepted_delivery_metrics_text(move=2, attack=8, loot=2),
                encoding="utf-8",
            )
            output = io.StringIO()

            with redirect_stdout(output):
                status = main(
                    [
                        "--runner-result",
                        str(runner_path),
                        "--server-accepted-metrics",
                        str(metrics_path),
                        "--runner-delivery-min-ratio",
                        "0.99",
                    ]
                )

        self.assertEqual(0, status)
        result = json.loads(output.getvalue())
        self.assertEqual("PASS", result["result_status"])
        self.assertEqual(["Attack", "Loot claim", "Move"], sorted(result["op_results"]))

    def test_cli_evaluates_primary_latency_metrics_json(self):
        metric_names = {
            "Move": "lol_rudp_move_receive_to_apply_latency_ms",
            "Attack": "lol_rudp_attack_receive_to_apply_latency_ms",
            "Loot claim": "lol_rudp_loot_claim_receive_to_apply_latency_ms",
        }
        metrics_text = "\n".join(
            latency_metric_text(metric_name, [(20, 95), (50, 100)], 100)
            for metric_name in metric_names.values()
        )
        with tempfile.TemporaryDirectory() as tmp:
            metrics_path = Path(tmp) / "lol_game.prom"
            metrics_path.write_text(metrics_text, encoding="utf-8")
            output = io.StringIO()

            with redirect_stdout(output):
                status = main(
                    [
                        "--primary-latency-metrics",
                        str(metrics_path),
                        "--primary-latency-min-samples",
                        "100",
                        "--primary-latency-p99-slo-ms",
                        "50",
                    ]
                )

        self.assertEqual(0, status)
        result = json.loads(output.getvalue())
        self.assertEqual("PASS", result["result_status"])
        self.assertEqual(
            ["Attack", "Loot claim", "Move"],
            sorted(result["op_results"].keys()),
        )


if __name__ == "__main__":
    unittest.main()
