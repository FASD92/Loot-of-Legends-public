import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ARCHITECTURE_DIR = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ARCHITECTURE_DIR))

from check_architecture import find_graph_violations, load_contract
from cmake_file_api import ensure_codemodel_query, read_target_graph


class CMakeFileApiTests(unittest.TestCase):
    def configure_fixture(self, name: str) -> dict[str, list[str]]:
        source = REPOSITORY_ROOT / "tests" / "architecture" / "fixtures" / name
        with tempfile.TemporaryDirectory() as temporary_directory:
            build = Path(temporary_directory) / "build"
            ensure_codemodel_query(build)
            subprocess.run(
                ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"],
                check=True,
                capture_output=True,
                text=True,
            )
            return read_target_graph(build)

    def finding_codes(self, fixture: str) -> set[str]:
        contract = load_contract(
            REPOSITORY_ROOT / "contracts" / "architecture" / "target-graph.v1.json"
        )
        return {
            finding["code"]
            for finding in find_graph_violations(
                self.configure_fixture(fixture), contract
            )
        }

    def test_reads_allowed_minimal_target_graph(self) -> None:
        expected_path = (
            REPOSITORY_ROOT
            / "tests"
            / "architecture"
            / "fixtures"
            / "allowed-minimal"
            / "expected.json"
        )
        expected = json.loads(expected_path.read_text())["targets"]

        actual = self.configure_fixture("allowed-minimal")

        self.assertEqual(actual["lol_game_server"], ["lol_runtime"])
        self.assertEqual(actual, expected)

    def test_rejects_forbidden_direct_dependency(self) -> None:
        self.assertIn(
            "FORBIDDEN_DIRECT_DEPENDENCY", self.finding_codes("forbidden-edge")
        )

    def test_rejects_dependency_cycle(self) -> None:
        graph_path = (
            REPOSITORY_ROOT
            / "tests"
            / "architecture"
            / "fixtures"
            / "dependency-cycle"
            / "graph.json"
        )
        contract = load_contract(
            REPOSITORY_ROOT / "contracts" / "architecture" / "target-graph.v1.json"
        )
        graph = json.loads(graph_path.read_text())["targets"]
        codes = {
            finding["code"] for finding in find_graph_violations(graph, contract)
        }

        self.assertIn("DEPENDENCY_CYCLE", codes)

    def test_rejects_unregistered_first_party_target(self) -> None:
        self.assertIn(
            "UNREGISTERED_FIRST_PARTY_TARGET",
            self.finding_codes("unregistered-target"),
        )


if __name__ == "__main__":
    unittest.main()
