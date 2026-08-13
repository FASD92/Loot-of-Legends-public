import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


VERIFIER = Path(__file__).with_name("verify_portfolio_claims.py")

EVIDENCE_PATHS = (
    "CMakeLists.txt",
    "README.md",
    "docs/recruiter-audit.md",
    "docs/verification.md",
    "game-server/platform/transport-tcp/include/lol/transport/tcp/TcpConnection.hpp",
    "game-server/platform/runtime-linux/include/lol/runtime/linux/EpollReactor.hpp",
    "game-server/platform/transport-rudp/include/lol/transport/rudp/ReliableQueue.hpp",
    "game-server/modules/game-flow/src/execution/RoomExecutionCell.hpp",
    "game-server/modules/battle/include/lol/battle/BattleLoadApi.hpp",
    "tests/transport-rudp/RudpDeliveryTests.cpp",
    "tests/integration/rudp-bind/RudpBindTests.cpp",
    "tools/load/loot_load/runner/runtime.py",
    "tools/load/tests/test_runner_runtime.py",
    "meta-server/src/main/java/com/fasd92/lootoflegends/meta/platform/mysql/JdbcSettlementInbox.java",
    "meta-server/src/test/java/com/fasd92/lootoflegends/meta/settlement/SettlementAcceptanceTest.java",
    "client/unity/Assets/LootOfLegends/Tests/EditMode/RudpCombatClientTests.cs",
)


class PortfolioClaimsVerifierTest(unittest.TestCase):
    def test_uses_only_tracked_files_and_reports_stale_claims(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)

            for relative_path in EVIDENCE_PATHS:
                path = root / relative_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")

            (root / "CMakeLists.txt").write_text(
                "add_test(NAME smoke COMMAND smoke)\n", encoding="utf-8"
            )
            load_test = root / "tools/load/tests/test_runner_runtime.py"
            load_test.write_text(
                "import unittest\n\n"
                "class RuntimeTest(unittest.TestCase):\n"
                "    def test_runtime(self):\n"
                "        pass\n",
                encoding="utf-8",
            )
            readme = root / "README.md"
            readme.write_text(
                "# Portfolio\n\n"
                "[RUDP evidence](tests/transport-rudp/RudpDeliveryTests.cpp)\n\n"
                "`game-server/modules/game-flow/src/execution/RoomExecutionCell.hpp`\n\n"
                "<!-- portfolio-test-count: ctest=1 load-unittest=1 -->\n"
                "Static registrations: 1 CTest test.\n",
                encoding="utf-8",
            )
            (root / ".gitignore").write_text("ignored/\n", encoding="utf-8")
            ignored = root / "ignored"
            ignored.mkdir()
            (ignored / "broken.md").write_text(
                "[broken](missing.md)\n" + "/" + "Users/private-user/project\n",
                encoding="utf-8",
            )
            (ignored / "secret.pem").write_text(
                "-----BEGIN " + "PRIVATE KEY-----\nnot-real\n",
                encoding="utf-8",
            )
            subprocess.run(["git", "add", "."], cwd=root, check=True)

            clean = self.run_verifier(root)
            self.assertEqual(0, clean.returncode, clean.stdout + clean.stderr)
            self.assertIn("PASS", clean.stdout)
            self.assertIn("static claim verification; tests were not executed", clean.stdout)

            readme.write_text(
                readme.read_text(encoding="utf-8").replace(
                    "<!-- portfolio-test-count: ctest=1 load-unittest=1 -->\n", ""
                ),
                encoding="utf-8",
            )
            missing_marker = self.run_verifier(root)
            self.assertEqual(1, missing_marker.returncode, missing_marker.stdout)
            self.assertIn("missing the portfolio test-count marker", missing_marker.stdout)

            readme.write_text(
                "# Portfolio\n\n"
                "[missing](docs/missing.md)\n\n"
                "`tests/missing.cpp`\n\n"
                "<!-- portfolio-test-count: ctest=2 load-unittest=2 -->\n"
                "Static registrations: 2 CTest tests.\n\n"
                + "/" + "Users/actual-user/private.txt\n\n"
                "-----BEGIN " + "PRIVATE KEY-----\n\n"
                "arn:" + "aws:iam::123456789012:role/private\n",
                encoding="utf-8",
            )
            forbidden = root / "docs/plans/internal.md"
            forbidden.parent.mkdir(parents=True, exist_ok=True)
            forbidden.write_text("private plan\n", encoding="utf-8")
            unlisted_suffix = root / "build.gradle.kts"
            unlisted_suffix.write_text(
                "privatePath = \"" + "/" + "Users/actual-user/private.txt\"\n",
                encoding="utf-8",
            )
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            stale = self.run_verifier(root)
            self.assertEqual(1, stale.returncode, stale.stdout + stale.stderr)
            self.assertIn("broken local Markdown link", stale.stdout)
            self.assertIn("missing backtick path", stale.stdout)
            self.assertIn("test-count mismatch", stale.stdout)
            self.assertIn("absolute user path", stale.stdout)
            self.assertIn("absolute user path in tracked file: build.gradle.kts", stale.stdout)
            self.assertIn("private key material", stale.stdout)
            self.assertIn("credential-like token", stale.stdout)
            self.assertIn("forbidden public path", stale.stdout)

    @staticmethod
    def run_verifier(root: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(VERIFIER), "--repo", str(root)],
            text=True,
            capture_output=True,
            check=False,
        )


if __name__ == "__main__":
    unittest.main()
