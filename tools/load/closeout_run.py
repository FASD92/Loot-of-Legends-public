#!/usr/bin/env python3
"""Classify one immutable fact document or verify a golden result fixture."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.load.loot_load.closeout.classifier import (  # noqa: E402
    EvaluationFacts,
    classify,
    write_classification,
)
from tools.load.loot_load.evidence.contracts import validate_document  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--run-input", type=Path)
    parser.add_argument("--facts", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.fixture is not None:
        expected = _read(args.fixture / "run-result.json")
        errors = validate_document("run-result", expected)
        if errors:
            print("FIXTURE INVALID: " + "; ".join(errors), file=sys.stderr)
            return 1
        print(f"FIXTURE PASS {expected['evaluationStatus']} {args.fixture}")
        return 0
    if None in (args.run_input, args.facts, args.output):
        parser.error("--run-input, --facts, and --output are required together")
    facts = _read(args.facts)
    result = classify(
        _read(args.run_input),
        EvaluationFacts(
            operator_aborted=facts["operatorAborted"],
            package_status=facts["packageStatus"],
            package_reason_codes=tuple(facts["packageReasonCodes"]),
            validity_reason_codes=tuple(facts["validityReasonCodes"]),
            correctness_reason_codes=tuple(facts["correctnessReasonCodes"]),
            samples={name: tuple(values) for name, values in facts["samples"].items()},
            expected_metric_scrapes=facts["expectedMetricScrapes"],
            successful_metric_scrapes=facts["successfulMetricScrapes"],
            planned_commands=facts["plannedCommands"],
            emitted_commands=facts["emittedCommands"],
            measurement_duration_ms=facts["measurementDurationMillis"],
            memory_limit_bytes=facts["memoryLimitBytes"],
            file_descriptor_limit=facts["fileDescriptorLimit"],
        ),
    )
    write_classification(args.output, result)
    print(f"{result['evaluationStatus']} {args.output}")
    return 0


def _read(path: Path) -> dict:
    if path.stat().st_size > 128 * 1024 * 1024:
        raise ValueError(f"artifact exceeds 128 MiB: {path}")
    value = json.loads(path.read_bytes())
    if not isinstance(value, dict):
        raise ValueError(f"artifact must be an object: {path}")
    return value


if __name__ == "__main__":
    raise SystemExit(main())
