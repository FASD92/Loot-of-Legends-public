#!/usr/bin/env python3
"""Generate Release 1 capacity-run run_config.json provenance."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence

from scripts.release1.capacity_report import sha256_file

try:
    import resource
except ImportError:  # pragma: no cover - resource is available on Linux/macOS.
    resource = None  # type: ignore[assignment]


APPROVED_ENV_KEYS = ("LANG", "LC_ALL", "LOL_CONFIG", "LOL_ENV", "PATH", "TZ")
ULIMIT_RESOURCES = (
    ("core", "RLIMIT_CORE"),
    ("nofile", "RLIMIT_NOFILE"),
    ("nproc", "RLIMIT_NPROC"),
    ("stack", "RLIMIT_STACK"),
)


def approved_environment(environment: Mapping[str, str] | None = None) -> dict[str, str]:
    source = os.environ if environment is None else environment
    return {key: str(source[key]) for key in APPROVED_ENV_KEYS if key in source}


def collect_ulimits() -> dict[str, int]:
    if resource is None:
        return {}

    values: dict[str, int] = {}
    for name, attr in ULIMIT_RESOURCES:
        if not hasattr(resource, attr):
            continue
        soft, hard = resource.getrlimit(getattr(resource, attr))
        values[f"{name}_soft"] = _rlimit_to_int(soft)
        values[f"{name}_hard"] = _rlimit_to_int(hard)
    return values


def build_execution_target(
    executable_path: Path | str,
    argv: Sequence[str],
    working_directory: Path | str,
    environment: Mapping[str, str] | None = None,
    ulimit: Mapping[str, int] | None = None,
) -> dict[str, Any]:
    return {
        "executable_path": str(executable_path),
        "argv": _string_list(argv, "argv"),
        "working_directory": str(working_directory),
        "binary_sha256": sha256_file(executable_path),
        "environment": approved_environment(environment),
        "ulimit": (
            collect_ulimits()
            if ulimit is None
            else {str(key): int(value) for key, value in ulimit.items()}
        ),
    }


def build_run_config(
    run_id: str,
    gate_config_path: Path | str,
    server: dict[str, Any],
    stress_runner: dict[str, Any],
) -> dict[str, Any]:
    return {
        "schema_version": "release1.capacity.run_config.v1",
        "run_id": run_id,
        "gate_config_sha256": sha256_file(gate_config_path),
        "execution": {
            "server": server,
            "stress_runner": stress_runner,
        },
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate Release 1 capacity-run run_config.json."
    )
    parser.add_argument("--run-id", required=True)
    parser.add_argument(
        "--gate-config",
        default=str(Path(__file__).with_name("gate_config.json")),
    )
    parser.add_argument("--server-executable", required=True)
    parser.add_argument("--server-working-directory", required=True)
    parser.add_argument("--server-argv-json", required=True)
    parser.add_argument("--stress-runner-executable", required=True)
    parser.add_argument("--stress-runner-working-directory", required=True)
    parser.add_argument("--stress-runner-argv-json", required=True)
    parser.add_argument("--output")
    return parser.parse_args(argv)


def main(
    argv: list[str] | None = None,
    environment: Mapping[str, str] | None = None,
    ulimit: Mapping[str, int] | None = None,
) -> int:
    args = parse_args(argv)
    try:
        server = build_execution_target(
            args.server_executable,
            _parse_argv_json(args.server_argv_json, "server argv"),
            args.server_working_directory,
            environment,
            ulimit,
        )
        stress_runner = build_execution_target(
            args.stress_runner_executable,
            _parse_argv_json(args.stress_runner_argv_json, "stress runner argv"),
            args.stress_runner_working_directory,
            environment,
            ulimit,
        )
        run_config = build_run_config(
            args.run_id,
            args.gate_config,
            server,
            stress_runner,
        )
        _write_json(run_config, args.output)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"run config generation failed: {error}", file=sys.stderr)
        return 2

    return 0


def _parse_argv_json(value: str, field_name: str) -> list[str]:
    parsed = json.loads(value)
    return _string_list(parsed, field_name)


def _string_list(values: Sequence[str], field_name: str) -> list[str]:
    if not isinstance(values, list) or not all(isinstance(item, str) for item in values):
        raise ValueError(f"{field_name} must be a JSON array of strings")
    return list(values)


def _rlimit_to_int(value: int) -> int:
    if resource is not None and value == resource.RLIM_INFINITY:
        return -1
    return int(value)


def _write_json(data: dict[str, Any], output_path: str | None) -> None:
    content = json.dumps(data, indent=2, sort_keys=True) + "\n"
    if output_path:
        Path(output_path).write_text(content, encoding="utf-8")
        return
    print(content, end="")


if __name__ == "__main__":
    raise SystemExit(main())
