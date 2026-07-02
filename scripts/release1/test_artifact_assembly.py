import json
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

from scripts.release1.artifact_assembly import assemble_artifact_bundle, main
from scripts.release1.capacity_report import load_json, sha256_file
from scripts.release1.test_capacity_report import make_run_config, make_summary


REPO_ROOT = Path(__file__).resolve().parents[2]
GATE_CONFIG_PATH = REPO_ROOT / "scripts" / "release1" / "gate_config.json"


class ArtifactAssemblyTests(unittest.TestCase):
    def test_bundle_copies_artifacts_and_writes_evaluation_and_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
            summary_path = root / "source-summary.json"
            run_config_path = root / "source-run-config.json"
            artifact_dir = root / "bundle"
            summary_path.write_text(json.dumps(make_summary()), encoding="utf-8")
            run_config_path.write_text(
                json.dumps(make_run_config(gate_config_sha256)),
                encoding="utf-8",
            )

            manifest = assemble_artifact_bundle(
                summary_path=summary_path,
                run_config_path=run_config_path,
                artifact_dir=artifact_dir,
                gate_config_path=GATE_CONFIG_PATH,
            )

            self.assertEqual("20260630-120000-clean-deadbee", manifest["run_id"])
            self.assertEqual("PASS", load_json(artifact_dir / "evaluation.json")["result_status"])
            self.assertEqual(
                make_summary(),
                load_json(artifact_dir / "summary.json"),
            )
            self.assertEqual(
                make_run_config(gate_config_sha256),
                load_json(artifact_dir / "run_config.json"),
            )
            self.assertEqual(
                sha256_file(artifact_dir / "summary.json"),
                manifest["artifacts"]["summary.json"]["sha256"],
            )
            self.assertEqual(
                sha256_file(artifact_dir / "run_config.json"),
                load_json(artifact_dir / "manifest.json")["artifacts"]["run_config.json"]["sha256"],
            )

    def test_bundle_rejects_mismatched_run_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
            summary = make_summary()
            run_config = make_run_config(gate_config_sha256)
            run_config["run_id"] = "different-run"
            summary_path = root / "summary.json"
            run_config_path = root / "run_config.json"
            summary_path.write_text(json.dumps(summary), encoding="utf-8")
            run_config_path.write_text(json.dumps(run_config), encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "run_id mismatch"):
                assemble_artifact_bundle(
                    summary_path=summary_path,
                    run_config_path=run_config_path,
                    artifact_dir=root / "bundle",
                    gate_config_path=GATE_CONFIG_PATH,
                )

    def test_bundle_includes_extra_artifacts_in_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
            summary_path = root / "summary.json"
            run_config_path = root / "run_config.json"
            runner_path = root / "runner.json"
            artifact_dir = root / "bundle"
            summary_path.write_text(json.dumps(make_summary()), encoding="utf-8")
            run_config_path.write_text(
                json.dumps(make_run_config(gate_config_sha256)),
                encoding="utf-8",
            )
            runner_path.write_text('{"valid": true}\n', encoding="utf-8")

            manifest = assemble_artifact_bundle(
                summary_path=summary_path,
                run_config_path=run_config_path,
                artifact_dir=artifact_dir,
                gate_config_path=GATE_CONFIG_PATH,
                extra_artifacts={"runner.json": runner_path},
            )

            self.assertEqual(
                {"valid": True},
                load_json(artifact_dir / "runner.json"),
            )
            self.assertEqual(
                sha256_file(artifact_dir / "runner.json"),
                manifest["artifacts"]["runner.json"]["sha256"],
            )

    def test_cli_creates_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gate_config_sha256 = sha256_file(GATE_CONFIG_PATH)
            summary_path = root / "summary.json"
            run_config_path = root / "run_config.json"
            artifact_dir = root / "bundle"
            summary_path.write_text(json.dumps(make_summary()), encoding="utf-8")
            run_config_path.write_text(
                json.dumps(make_run_config(gate_config_sha256)),
                encoding="utf-8",
            )

            stdout = StringIO()
            with redirect_stdout(stdout):
                status = main(
                    [
                        "--summary",
                        str(summary_path),
                        "--run-config",
                        str(run_config_path),
                        "--gate-config",
                        str(GATE_CONFIG_PATH),
                        "--artifact-dir",
                        str(artifact_dir),
                    ]
                )

            self.assertEqual(0, status)
            self.assertEqual("PASS", load_json(artifact_dir / "evaluation.json")["result_status"])
            self.assertTrue((artifact_dir / "manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
