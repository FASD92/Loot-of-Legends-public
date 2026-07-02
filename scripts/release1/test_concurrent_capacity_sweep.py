import json
import tempfile
import unittest
from pathlib import Path

from scripts.release1.concurrent_capacity_sweep import run_concurrent_capacity_sweep


class ConcurrentCapacitySweepTests(unittest.TestCase):
    def test_sweep_runs_counts_and_writes_boundary_summary(self):
        calls = []

        def fake_probe(**kwargs):
            calls.append(kwargs)
            participants = kwargs["participants"]
            passed = participants <= 20
            return {
                "valid": passed,
                "reason": "" if passed else "group_or_delivery_failed",
                "evaluation": {"result_status": "PASS" if passed else "FAIL"},
                "valid_groups": participants // kwargs["room_size"] if passed else 1,
                "group_count": participants // kwargs["room_size"],
                "runner_delivery_evaluation": {"result_status": "PASS"},
                "server_accepted_delivery_evaluation": {"result_status": "PASS"},
                "primary_latency_evaluation": {"result_status": "PASS"},
            }

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ports = iter([41010, 41020, 41030])
            result = run_concurrent_capacity_sweep(
                run_id_prefix="20260701-concurrent",
                artifact_root=root,
                server_bin=Path("/fake/lol_server"),
                host="127.0.0.1",
                participant_counts=[10, 20, 30],
                room_size=10,
                timeout_sec=2.0,
                auth_token_prefix="dev-session:",
                move_duration_sec=0.2,
                move_rate_hz=10.0,
                attack_duration_sec=0.4,
                attack_rate_hz=10.0,
                battle_cycles=2,
                rudp_handshake_settle_sec=0.1,
                post_runner_metrics_settle_sec=0.5,
                allocate_port=lambda host: next(ports),
                run_one=fake_probe,
            )

            saved = json.loads((root / "concurrent_capacity_sweep.json").read_text())

        self.assertEqual(20, result["highest_passing_participants"])
        self.assertEqual(30, result["first_non_passing_participants"])
        self.assertEqual(result, saved)
        self.assertEqual([10, 20, 30], [call["participants"] for call in calls])
        self.assertEqual([41010, 41020, 41030], [call["tcp_port"] for call in calls])
        self.assertEqual(calls[0]["tcp_port"], calls[0]["udp_port"])
        self.assertEqual(
            str(root / "20260701-concurrent-n10"),
            str(calls[0]["artifact_dir"]),
        )
        self.assertEqual(str(Path("/fake/lol_server")), calls[0]["server_argv"][0])
        self.assertEqual("--metrics-textfile", calls[0]["server_argv"][2])
        self.assertEqual(
            str(root / "20260701-concurrent-n10" / "lol_game.prom"),
            calls[0]["server_argv"][3],
        )
        self.assertEqual(0.1, calls[0]["rudp_handshake_settle_sec"])

    def test_sweep_records_exception_and_continues(self):
        def fake_probe(**kwargs):
            if kwargs["participants"] == 20:
                raise ValueError("runner refused count")
            return {
                "valid": True,
                "reason": "",
                "evaluation": {"result_status": "PASS"},
                "valid_groups": 1,
                "group_count": 1,
                "runner_delivery_evaluation": {"result_status": "PASS"},
                "server_accepted_delivery_evaluation": {"result_status": "PASS"},
                "primary_latency_evaluation": {"result_status": "PASS"},
            }

        with tempfile.TemporaryDirectory() as tmp:
            ports = iter([41010, 41020])
            result = run_concurrent_capacity_sweep(
                run_id_prefix="20260701-concurrent",
                artifact_root=Path(tmp),
                server_bin=Path("/fake/lol_server"),
                host="127.0.0.1",
                participant_counts=[10, 20],
                room_size=10,
                timeout_sec=2.0,
                allocate_port=lambda host: next(ports),
                run_one=fake_probe,
            )

        self.assertEqual(10, result["highest_passing_participants"])
        self.assertEqual(20, result["first_non_passing_participants"])
        self.assertEqual("INVALID", result["runs"][1]["result_status"])
        self.assertEqual("runner refused count", result["runs"][1]["reason"])


if __name__ == "__main__":
    unittest.main()
