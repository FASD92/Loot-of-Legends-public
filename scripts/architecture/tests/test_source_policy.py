import sys
import unittest
from pathlib import Path

ARCHITECTURE_DIR = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ARCHITECTURE_DIR))

from source_policy import scan_source_policy


class SourcePolicyTests(unittest.TestCase):
    def fixture_findings(self, name: str) -> list[dict]:
        fixture = REPOSITORY_ROOT / "tests" / "architecture" / "fixtures" / name
        _, findings = scan_source_policy(fixture)
        return findings

    def test_rejects_private_include_boundary_violations(self) -> None:
        findings = self.fixture_findings("private-include")
        codes = {finding["code"] for finding in findings}

        self.assertIn("CROSS_MODULE_PRIVATE_INCLUDE", codes)
        self.assertIn("PARENT_PATH_BOUNDARY_ESCAPE", codes)
        self.assertIn("PUBLIC_HEADER_PRIVATE_TYPE_LEAK", codes)

    def test_rejects_global_cmake_commands_and_recursive_glob(self) -> None:
        findings = self.fixture_findings("forbidden-cmake-command")
        commands = {
            finding["command"]
            for finding in findings
            if finding["code"] == "FORBIDDEN_GLOBAL_CMAKE_COMMAND"
        }
        codes = {finding["code"] for finding in findings}

        self.assertEqual(commands, {"include_directories", "link_libraries"})
        self.assertIn("FIRST_PARTY_RECURSIVE_GLOB", codes)

    def test_repository_source_policy_is_clean(self) -> None:
        files_scanned, findings = scan_source_policy(REPOSITORY_ROOT)

        self.assertGreater(files_scanned, 0)
        self.assertEqual(findings, [])


if __name__ == "__main__":
    unittest.main()
