"""Check published record integrity; this does not rerun the historical experiments."""
from __future__ import annotations

import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CASES = ROOT / "docs" / "cases"


def load(name: str) -> dict:
    return json.loads((CASES / name).read_text(encoding="utf-8"))


class CaseRecordTests(unittest.TestCase):
    def test_manifest_covers_exact_case_files(self) -> None:
        manifest = load("publication-manifest.json")
        expected = {entry["path"] for entry in manifest["files"]}
        actual = {
            path.relative_to(ROOT).as_posix()
            for path in CASES.rglob("*")
            if path.is_file() and path.name != "publication-manifest.json"
        }
        self.assertEqual(expected, actual)
        self.assertEqual(len(expected), len(manifest["files"]))
        for entry in manifest["files"]:
            path = (ROOT / entry["path"]).resolve()
            self.assertTrue(path.is_relative_to(CASES.resolve()))
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), entry["sha256"], entry["path"])

    def test_receive_comparison_preserves_distinct_sources(self) -> None:
        data = load("records/receive-comparison.json")
        before, after = data["baseline"], data["candidate"]
        self.assertNotEqual(before["sourceSha"], after["sourceSha"])
        self.assertEqual(before["profile"], after["profile"])
        for run in (before, after):
            profile = run["profile"]
            self.assertEqual(profile["participantCount"], profile["roomCount"] * profile["participantsPerRoom"])
            self.assertEqual(len(run["sourceSha"]), 40)
            self.assertLessEqual(run["roomFailures"]["count"], profile["roomCount"])
        self.assertEqual(before["evaluationStatus"], "INVALID")
        self.assertEqual(after["evaluationStatus"], "PASS")
        self.assertFalse(data["publicRuntimeRetested"])

    def test_capacity_keeps_original_and_posthoc_separate(self) -> None:
        data = load("records/capacity-5250.json")
        profile, measurement = data["profile"], data["measurement"]
        self.assertEqual(profile["participantCount"], profile["roomCount"] * profile["participantsPerRoom"])
        self.assertEqual(measurement["completedRooms"] + measurement["failedRooms"], profile["roomCount"])
        self.assertFalse(profile["capacityClaimEligible"])
        self.assertEqual(data["originalReceipt"]["evaluationStatus"], "INVALID")
        self.assertEqual(data["originalReceipt"]["packageStatus"], "INCOMPLETE")
        self.assertEqual(data["postHocReview"]["evaluationStatus"], "PASS")
        self.assertEqual(data["scope"]["dbSettlementLoadInclusion"], "UNKNOWN")
        self.assertFalse(data["scope"]["publicRootCapacityClaim"])

    def test_recovery_observations_have_no_additional_assets(self) -> None:
        data = load("records/recovery-db.json")
        first, *later = data["observations"]
        self.assertEqual(len(later), 3)
        for observation in data["observations"]:
            settlement = observation["settlement"]
            self.assertEqual(settlement["total"], settlement["applied"] + settlement["pending"])
        for observation in later:
            for name in ("settlement", "inventory", "wallet"):
                self.assertEqual(first[name], observation[name])
        self.assertTrue(all(value == 0 for value in data["deltaAfterTerminalRestarts"].values()))
        self.assertFalse(data["scope"]["retestedForPublication"])

    def test_records_exclude_operational_identifiers(self) -> None:
        forbidden = {"account", "token", "password", "credential", "endpoint", "ip", "playerId", "sessionId", "roomGroupId", "archive"}
        def visit(value: object) -> None:
            if isinstance(value, dict):
                self.assertFalse(forbidden.intersection(value))
                for child in value.values():
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)
        for path in sorted((CASES / "records").glob("*.json")):
            visit(json.loads(path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
