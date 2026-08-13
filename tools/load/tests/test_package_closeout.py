from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.load.loot_load.evidence.contracts import validate_document


try:
    from tools.load.loot_load.evidence.package import (
        EvidenceSanitizer,
        close_package,
        scan_public_bytes,
        verify_package,
    )
except ModuleNotFoundError:
    EvidenceSanitizer = None
    close_package = None
    scan_public_bytes = None
    verify_package = None


class PackageCloseoutTests(unittest.TestCase):
    def setUp(self):
        if close_package is None:
            self.fail("evidence package module is absent")

    def test_sanitizer_removes_identity_secret_endpoint_path_and_stack(self):
        raw = json.dumps(
            {
                "sessionId": 77,
                "accountId": "00000000-0000-4000-8000-000000000001",
                "nickname": "real-player",
                "credential": "secret-token-value",
                "cookie": "session=secret-cookie",
                "email": "player@example.com",
                "privateEndpoint": "https://10.1.2.3:8443/internal",
                "secretPath": "/private-fixture/credentials",
                "stackTrace": "at com.example.Secret.run(Secret.java:7)",
                "message": "credential=CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
                "completedCycles": 2,
            }
        ).encode()
        sanitizer = EvidenceSanitizer()
        sanitized = sanitizer.sanitize_bytes(Path("raw/fact.json"), raw)
        value = json.loads(sanitized)
        self.assertEqual("participant-0001", value["sessionId"])
        self.assertEqual("participant-0001", value["accountId"])
        self.assertEqual("participant-0001", value["nickname"])
        self.assertEqual(2, value["completedCycles"])
        self.assertEqual([], scan_public_bytes(sanitized))
        for forbidden in (
            b"secret-token-value",
            b"secret-cookie",
            b"player@example.com",
            b"10.1.2.3",
            b"/private-fixture",
            b"com.example.Secret",
            b"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
        ):
            self.assertNotIn(forbidden, sanitized)

    def test_complete_package_has_separate_digests_checksums_and_public_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / "run"
            (run / "raw").mkdir(parents=True)
            (run / "raw" / "runner-facts.jsonl").write_text(
                '{"sequence":1,"sessionId":7,"completedCycles":2}\n'
            )
            (run / "manifest").mkdir()
            (run / "manifest" / "run-input.json").write_text('{"runId":"run-1"}\n')
            output = root / "package"
            package = close_package(
                run,
                output,
                run_id="run-1",
                required_paths=("raw/runner-facts.jsonl", "manifest/run-input.json"),
            )
            self.assertEqual("COMPLETE", package["packageStatus"])
            self.assertNotEqual(package["rawBundleDigest"], package["sanitizedBundleDigest"])
            self.assertEqual([], validate_document("evidence-package", package))
            self.assertEqual("COMPLETE", verify_package(output)["packageStatus"])
            self.assertTrue((output / "checksums" / "SHA256SUMS").is_file())
            self.assertTrue((output / "restricted" / "raw" / "runner-facts.jsonl").is_file())
            summary = (output / "public" / "summary.md").read_bytes()
            self.assertEqual([], scan_public_bytes(summary))
            sanitized = (output / "sanitized" / "raw" / "runner-facts.jsonl").read_text()
            self.assertIn("participant-0001", sanitized)
            self.assertNotIn('"sessionId":7', sanitized)

    def test_missing_input_is_incomplete_and_existing_output_is_never_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / "run"
            run.mkdir()
            output = root / "package"
            package = close_package(
                run,
                output,
                run_id="run-2",
                required_paths=("raw/runner-facts.jsonl",),
            )
            self.assertEqual("INCOMPLETE", package["packageStatus"])
            self.assertEqual(["PACKAGE_FILE_MISSING"], package["reasonCodes"])
            self.assertEqual(["raw/runner-facts.jsonl"], package["missingPaths"])
            with self.assertRaises(FileExistsError):
                close_package(
                    run,
                    output,
                    run_id="run-2",
                    required_paths=("raw/runner-facts.jsonl",),
                )

    def test_checksum_corruption_is_package_corrupt_not_evaluation_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / "run"
            (run / "raw").mkdir(parents=True)
            (run / "raw" / "runner-facts.jsonl").write_text('{"sequence":1}\n')
            output = root / "package"
            close_package(
                run,
                output,
                run_id="run-3",
                required_paths=("raw/runner-facts.jsonl",),
            )
            target = output / "sanitized" / "raw" / "runner-facts.jsonl"
            target.write_text("tampered\n")
            verified = verify_package(output)
            self.assertEqual("CORRUPT", verified["packageStatus"])
            self.assertEqual(["PACKAGE_CHECKSUM_MISMATCH"], verified["reasonCodes"])
            self.assertEqual([], validate_document("evidence-package", verified))

    def test_missing_checksum_file_is_incomplete(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / "run"
            (run / "raw").mkdir(parents=True)
            (run / "raw" / "runner-facts.jsonl").write_text('{"sequence":1}\n')
            output = root / "package"
            close_package(
                run,
                output,
                run_id="run-4",
                required_paths=("raw/runner-facts.jsonl",),
            )
            (output / "checksums" / "SHA256SUMS").unlink()
            verified = verify_package(output)
            self.assertEqual("INCOMPLETE", verified["packageStatus"])
            self.assertEqual(["checksums/SHA256SUMS"], verified["missingPaths"])


if __name__ == "__main__":
    unittest.main()
