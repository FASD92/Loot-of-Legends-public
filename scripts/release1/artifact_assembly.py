#!/usr/bin/env python3
"""Assemble Release 1 capacity-run artifacts into one directory."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Any, Mapping

from scripts.release1.capacity_report import (
    evaluate_from_paths,
    load_json,
    sha256_file,
)


def assemble_artifact_bundle(
    summary_path: Path | str,
    run_config_path: Path | str,
    artifact_dir: Path | str,
    gate_config_path: Path | str,
    extra_artifacts: Mapping[str, Path | str] | None = None,
) -> dict[str, Any]:
    summary = load_json(summary_path)
    run_config = load_json(run_config_path)
    run_id = summary.get("run_id", "")
    if run_id != run_config.get("run_id", ""):
        raise ValueError("run_id mismatch between summary.json and run_config.json")

    evaluation = evaluate_from_paths(summary_path, run_config_path, gate_config_path)
    target_dir = Path(artifact_dir)
    target_dir.mkdir(parents=True, exist_ok=True)

    copied_summary = target_dir / "summary.json"
    copied_run_config = target_dir / "run_config.json"
    evaluation_path = target_dir / "evaluation.json"
    manifest_path = target_dir / "manifest.json"

    _copy_artifact(summary_path, copied_summary)
    _copy_artifact(run_config_path, copied_run_config)
    _write_json(evaluation_path, evaluation)

    artifacts = {
        "summary.json": _artifact_entry(copied_summary, summary_path),
        "run_config.json": _artifact_entry(copied_run_config, run_config_path),
        "evaluation.json": _artifact_entry(evaluation_path, evaluation_path),
    }
    for name, source_path in (extra_artifacts or {}).items():
        copied_extra = target_dir / name
        _copy_artifact(source_path, copied_extra)
        artifacts[name] = _artifact_entry(copied_extra, source_path)

    manifest = {
        "schema_version": "release1.capacity.artifact_manifest.v1",
        "run_id": run_id,
        "artifacts": artifacts,
    }
    _write_json(manifest_path, manifest)
    return manifest


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Assemble Release 1 capacity-run artifacts."
    )
    parser.add_argument("--summary", required=True)
    parser.add_argument("--run-config", required=True)
    parser.add_argument(
        "--gate-config",
        default=str(Path(__file__).with_name("gate_config.json")),
    )
    parser.add_argument("--artifact-dir", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        manifest = assemble_artifact_bundle(
            summary_path=args.summary,
            run_config_path=args.run_config,
            artifact_dir=args.artifact_dir,
            gate_config_path=args.gate_config,
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"artifact assembly failed: {error}", file=sys.stderr)
        return 2

    print(json.dumps(manifest, sort_keys=True))
    return 0


def _artifact_entry(path: Path, source_path: Path | str) -> dict[str, str]:
    return {
        "path": str(path),
        "source_path": str(source_path),
        "sha256": sha256_file(path),
    }


def _copy_artifact(source_path: Path | str, target_path: Path) -> None:
    source = Path(source_path)
    target_path.parent.mkdir(parents=True, exist_ok=True)
    if source.resolve() == target_path.resolve():
        return
    shutil.copy2(source, target_path)


def _write_json(path: Path, data: dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
