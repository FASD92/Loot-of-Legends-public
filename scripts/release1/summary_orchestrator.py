#!/usr/bin/env python3
"""Generate the minimal Release 1 capacity-run summary.json."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence


def build_summary(
    run_id: str,
    completed_official_window: bool,
    operator_aborted: bool = False,
    invalid_category: str = "",
    invalid_reason: str = "",
    failed_hard_gate_categories: Sequence[str] | None = None,
    regression_comparisons: Sequence[Mapping[str, str]] | None = None,
    gate_evaluations: Sequence[Mapping[str, Any]] | None = None,
) -> dict[str, Any]:
    failed_categories = list(failed_hard_gate_categories or [])
    resolved_invalid_category = invalid_category
    resolved_invalid_reason = invalid_reason
    for evaluation in gate_evaluations or []:
        status = str(evaluation.get("result_status", ""))
        if status == "INVALID" and (
            resolved_invalid_category == "" and resolved_invalid_reason == ""
        ):
            resolved_invalid_category = str(evaluation.get("invalid_category", ""))
            resolved_invalid_reason = str(evaluation.get("reason", ""))
        if status == "FAIL":
            for category in evaluation.get("failed_hard_gate_categories", []):
                category_name = str(category)
                if category_name and category_name not in failed_categories:
                    failed_categories.append(category_name)

    comparisons = [
        {
            "name": str(comparison.get("name", "")),
            "gate_config_sha256": str(comparison.get("gate_config_sha256", "")),
        }
        for comparison in (regression_comparisons or [])
    ]
    return {
        "schema_version": "release1.capacity.summary.v1",
        "run_id": run_id,
        "completed_official_window": completed_official_window,
        "operator_aborted": operator_aborted,
        "validity": {
            "valid": resolved_invalid_category == "" and resolved_invalid_reason == "",
            "invalid_category": resolved_invalid_category,
            "reason": resolved_invalid_reason,
        },
        "hard_gates": {
            "all_passed": len(failed_categories) == 0,
            "failed_categories": failed_categories,
        },
        "regression_comparisons": comparisons,
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate Release 1 capacity-run summary.json."
    )
    parser.add_argument("--run-id", required=True)
    parser.add_argument(
        "--completed-official-window",
        action="store_true",
        help="Set when the official measurement window completed.",
    )
    parser.add_argument("--operator-aborted", action="store_true")
    parser.add_argument("--invalid-category", default="")
    parser.add_argument("--invalid-reason", default="")
    parser.add_argument("--failed-hard-gate", action="append", default=[])
    parser.add_argument(
        "--regression-comparison-json",
        action="append",
        default=[],
        help="JSON object with name and gate_config_sha256.",
    )
    parser.add_argument(
        "--gate-evaluation-json",
        action="append",
        default=[],
        help="JSON object with result_status and optional gate details.",
    )
    parser.add_argument(
        "--gate-evaluation-file",
        action="append",
        default=[],
        help="Path to a JSON object with result_status and optional gate details.",
    )
    parser.add_argument("--output")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        summary = build_summary(
            run_id=args.run_id,
            completed_official_window=args.completed_official_window,
            operator_aborted=args.operator_aborted,
            invalid_category=args.invalid_category,
            invalid_reason=args.invalid_reason,
            failed_hard_gate_categories=args.failed_hard_gate,
            regression_comparisons=[
                _parse_regression_comparison(raw)
                for raw in args.regression_comparison_json
            ],
            gate_evaluations=[
                _parse_gate_evaluation(raw) for raw in args.gate_evaluation_json
            ]
            + [
                _load_gate_evaluation_file(path)
                for path in args.gate_evaluation_file
            ],
        )
        _write_json(summary, args.output)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"summary generation failed: {error}", file=sys.stderr)
        return 2
    return 0


def _parse_regression_comparison(raw: str) -> dict[str, str]:
    parsed = json.loads(raw)
    if not isinstance(parsed, dict):
        raise ValueError("regression comparison must be a JSON object")
    return {
        "name": str(parsed.get("name", "")),
        "gate_config_sha256": str(parsed.get("gate_config_sha256", "")),
    }


def _parse_gate_evaluation(raw: str) -> dict[str, Any]:
    parsed = json.loads(raw)
    if not isinstance(parsed, dict):
        raise ValueError("gate evaluation must be a JSON object")
    return parsed


def _load_gate_evaluation_file(path: str) -> dict[str, Any]:
    parsed = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(parsed, dict):
        raise ValueError("gate evaluation file must contain a JSON object")
    return parsed


def _write_json(data: dict[str, Any], output_path: str | None) -> None:
    content = json.dumps(data, indent=2, sort_keys=True) + "\n"
    if output_path:
        Path(output_path).write_text(content, encoding="utf-8")
        return
    print(content, end="")


if __name__ == "__main__":
    raise SystemExit(main())
