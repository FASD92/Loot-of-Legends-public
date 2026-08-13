from __future__ import annotations

import json
import unittest
from pathlib import Path

from tools.load.loot_load.evidence.contracts import validate_document


ROOT = Path(__file__).resolve().parents[3]
PROFILES = ROOT / "tools" / "load" / "profiles"


class WorkloadProfileTests(unittest.TestCase):
    def test_required_profile_set_is_exact_and_contract_valid(self):
        expected = {
            "release-functional-10p-v1.json",
            "release-smoke-10p-v1.json",
            "release-candidate-20p-v1.json",
            "official-capacity-v1.template.json",
            "resilience-soak-v1.template.json",
        }
        self.assertEqual(expected, {path.name for path in PROFILES.glob("*.json")})
        values = {}
        for path in PROFILES.glob("*.json"):
            value = json.loads(path.read_text())
            self.assertEqual([], validate_document("workload-profile", value), path.name)
            values[path.name] = value
        self.assertEqual((10, 1, 2, 180), self._core(values["release-functional-10p-v1.json"]))
        self.assertEqual((10, 1, 2, 120), self._core(values["release-smoke-10p-v1.json"]))
        self.assertEqual((20, 2, 4, 420), self._core(values["release-candidate-20p-v1.json"]))
        official = values["official-capacity-v1.template.json"]
        self.assertEqual((10, 1, 30, 3840), self._core(official))
        self.assertTrue(official["capacityClaimEligible"])
        resilience = values["resilience-soak-v1.template.json"]
        self.assertEqual(14400, resilience["measurementSeconds"])
        self.assertFalse(resilience["capacityClaimEligible"])

    @staticmethod
    def _core(value):
        return (
            value["participantCount"],
            value["roomCount"],
            value["requiredCyclesPerRoom"],
            value["overallDeadlineSeconds"],
        )


if __name__ == "__main__":
    unittest.main()
