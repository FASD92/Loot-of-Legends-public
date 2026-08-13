import argparse
import json
import subprocess
import sys
from pathlib import Path

from cmake_file_api import ensure_codemodel_query, read_target_graph
from source_policy import scan_source_policy


def load_contract(path: Path) -> dict:
    contract = json.loads(path.read_text())
    if contract.get("status") != "Accepted":
        raise ValueError("Architecture contract must be Accepted")
    if not isinstance(contract.get("planned_targets"), list):
        raise ValueError("Architecture contract has no planned_targets")
    if not isinstance(contract.get("allowed_direct_dependencies"), dict):
        raise ValueError("Architecture contract has no allowed_direct_dependencies")
    return contract


def _dependency_cycles(graph: dict[str, list[str]]) -> list[list[str]]:
    cycles: set[tuple[str, ...]] = set()

    def visit(node: str, path: list[str]) -> None:
        if node in path:
            cycles.add(tuple(sorted(path[path.index(node) :])))
            return
        for dependency in graph.get(node, []):
            if dependency in graph:
                visit(dependency, [*path, node])

    for target in sorted(graph):
        visit(target, [])
    return [list(cycle) for cycle in sorted(cycles)]


def find_graph_violations(graph: dict[str, list[str]], contract: dict) -> list[dict]:
    planned_targets = set(contract["planned_targets"])
    allowed_dependencies = contract["allowed_direct_dependencies"]
    findings: list[dict] = []

    for target in sorted(graph):
        if target not in planned_targets:
            findings.append(
                {"code": "UNREGISTERED_FIRST_PARTY_TARGET", "target": target}
            )
        allowed = set(allowed_dependencies.get(target, []))
        for dependency in graph[target]:
            if dependency not in allowed:
                findings.append(
                    {
                        "code": "FORBIDDEN_DIRECT_DEPENDENCY",
                        "dependency": dependency,
                        "target": target,
                    }
                )

    for cycle in _dependency_cycles(graph):
        findings.append({"code": "DEPENDENCY_CYCLE", "targets": cycle})

    return sorted(findings, key=lambda finding: json.dumps(finding, sort_keys=True))


def write_report(
    path: Path,
    graph: dict[str, list[str]],
    source_file_count: int,
    findings: list[dict],
) -> None:
    report = {
        "schemaVersion": 1,
        "status": "PASS" if not findings else "FAIL",
        "targets": graph,
        "findings": findings,
        "summary": {
            "edgeCount": sum(len(dependencies) for dependencies in graph.values()),
            "findingCount": len(findings),
            "sourceFileCount": source_file_count,
            "targetCount": len(graph),
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--contract", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    source_root = arguments.source_root.resolve()
    build_directory = arguments.build_dir.resolve()
    try:
        contract = load_contract(arguments.contract.resolve())
        ensure_codemodel_query(build_directory)
        subprocess.run(
            ["cmake", "-S", str(source_root), "-B", str(build_directory)],
            check=True,
        )
        graph = read_target_graph(build_directory)
        source_file_count, source_findings = scan_source_policy(source_root)
        findings = find_graph_violations(graph, contract) + source_findings
        findings.sort(key=lambda finding: json.dumps(finding, sort_keys=True))
        write_report(
            arguments.report.resolve(), graph, source_file_count, findings
        )
    except (OSError, ValueError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"architecture checker error: {error}", file=sys.stderr)
        return 2

    print(
        f"Architecture check {'PASSED' if not findings else 'FAILED'}: "
        f"{len(graph)} targets, "
        f"{sum(len(dependencies) for dependencies in graph.values())} edges, "
        f"{source_file_count} source files, "
        f"{len(findings)} findings"
    )
    return 0 if not findings else 1


if __name__ == "__main__":
    raise SystemExit(main())
