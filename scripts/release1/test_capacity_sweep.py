import json
import tempfile
import unittest
from pathlib import Path

from scripts.release1.capacity_sweep import (
    parse_participant_counts,
    run_capacity_sweep,
)


class CapacitySweepTests(unittest.TestCase):
    def test_sweep_runs_counts_and_writes_highest_pass(self):
        calls = []

        def fake_official_run(**kwargs):
            calls.append(kwargs)
            participants = kwargs["participants"]
            status = "PASS" if participants <= 4 else "FAIL"
            return {
                "valid": status == "PASS",
                "reason": "" if status == "PASS" else "primary_latency_p99_exceeded",
                "run_id": kwargs["run_id"],
                "artifact_dir": str(kwargs["artifact_dir"]),
                "official_window": {"completed": True},
                "evaluation": {"result_status": status},
            }

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ports = iter([41002, 41004, 41006])
            result = run_capacity_sweep(
                run_id_prefix="20260701-010208-sweep",
                artifact_root=root,
                server_bin=Path("/fake/lol_server"),
                host="127.0.0.1",
                participant_counts=[2, 4, 6],
                room_size=2,
                timeout_sec=2.0,
                auth_token_prefix="dev-session:",
                official_window_sec=0.0,
                move_duration_sec=0.03,
                move_rate_hz=100.0,
                attack_duration_sec=0.04,
                attack_rate_hz=100.0,
                battle_cycles=2,
                allocate_port=lambda host: next(ports),
                run_one=fake_official_run,
            )

            self.assertEqual(4, result["highest_passing_participants"])
            self.assertEqual(3, len(result["runs"]))
            self.assertEqual([2, 4, 6], [call["participants"] for call in calls])
            self.assertEqual(
                str(root / "20260701-010208-sweep-n2"),
                str(calls[0]["artifact_dir"]),
            )
            self.assertEqual(str(Path("/fake/lol_server")), calls[0]["server_argv"][0])
            self.assertEqual("--metrics-textfile", calls[0]["server_argv"][2])
            self.assertEqual(
                str(root / "20260701-010208-sweep-n2" / "lol_game.prom"),
                calls[0]["server_argv"][3],
            )
            self.assertEqual(calls[0]["tcp_port"], calls[0]["udp_port"])
            self.assertEqual(2, calls[0]["battle_cycles"])

            saved = json.loads((root / "capacity_sweep.json").read_text())
            self.assertEqual(result, saved)

    def test_sweep_records_run_exception_and_continues(self):
        def fake_official_run(**kwargs):
            if kwargs["participants"] == 4:
                raise ValueError("server refused test load")
            return {
                "valid": True,
                "reason": "",
                "run_id": kwargs["run_id"],
                "artifact_dir": str(kwargs["artifact_dir"]),
                "official_window": {"completed": True},
                "evaluation": {"result_status": "PASS"},
            }

        with tempfile.TemporaryDirectory() as tmp:
            ports = iter([41002, 41004])
            result = run_capacity_sweep(
                run_id_prefix="20260701-010209-sweep",
                artifact_root=Path(tmp),
                server_bin=Path("/fake/lol_server"),
                host="127.0.0.1",
                participant_counts=[2, 4],
                room_size=2,
                timeout_sec=2.0,
                allocate_port=lambda host: next(ports),
                run_one=fake_official_run,
            )

        self.assertEqual(2, result["highest_passing_participants"])
        self.assertEqual("INVALID", result["runs"][1]["result_status"])
        self.assertEqual("server refused test load", result["runs"][1]["reason"])

    def test_parse_participant_counts_rejects_bad_values(self):
        self.assertEqual([2, 4, 6], parse_participant_counts("2, 4,6"))
        for raw in ["", "2,2", "0", "-1", "abc"]:
            with self.subTest(raw=raw):
                with self.assertRaises(ValueError):
                    parse_participant_counts(raw)


if __name__ == "__main__":
    unittest.main()
