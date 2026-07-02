import json
import sys
import tempfile
import unittest
from pathlib import Path

from scripts.release1.concurrent_capacity_probe import (
    run_concurrent_capacity_probe,
    run_concurrent_runner_groups,
)


class ConcurrentCapacityProbeTests(unittest.TestCase):
    def test_capacity_probe_includes_primary_latency_gate(self):
        def fake_run_group(**kwargs):
            return {
                "valid": True,
                "reason": "",
                "rudp_move_target_sent": 2,
                "rudp_move_sent": 2,
                "rudp_attack_target_sent": 4,
                "rudp_attack_sent": 4,
                "rudp_click_loot_target_sent": 1,
                "rudp_click_loot_sent": 1,
                "rudp_click_loot_server_accept_target_sent": 1,
                "tcp_arena_gameplay_start_received": 10,
                "tcp_battle_final_ranking_received": 10,
                "tcp_lobby_return_received": 10,
            }

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_server = root / "fake_server.py"
            fake_server.write_text(_fake_metrics_server_source(), encoding="utf-8")

            result = run_concurrent_capacity_probe(
                artifact_dir=root / "artifact",
                server_argv=[
                    sys.executable,
                    str(fake_server),
                    str(root / "artifact" / "lol_game.prom"),
                ],
                host="127.0.0.1",
                tcp_port=41000,
                participants=10,
                room_size=10,
                timeout_sec=2.0,
                udp_port=41000,
                auth_token_prefix="dev-session:",
                battle_cycles=1,
                primary_latency_min_samples=1,
                primary_latency_p99_slo_ms=50.0,
                ready_timeout_sec=2.0,
                shutdown_timeout_sec=1.0,
                run_group=fake_run_group,
            )

            saved = json.loads((root / "artifact" / "probe.json").read_text())

        self.assertTrue(result["valid"])
        self.assertEqual("PASS", result["primary_latency_evaluation"]["result_status"])
        self.assertEqual("PASS", saved["primary_latency_evaluation"]["result_status"])

    def test_capacity_probe_fails_when_primary_latency_exceeds_slo(self):
        def fake_run_group(**kwargs):
            return {
                "valid": True,
                "reason": "",
                "rudp_move_target_sent": 2,
                "rudp_move_sent": 2,
                "rudp_attack_target_sent": 4,
                "rudp_attack_sent": 4,
                "rudp_click_loot_target_sent": 1,
                "rudp_click_loot_sent": 1,
                "rudp_click_loot_server_accept_target_sent": 1,
                "tcp_arena_gameplay_start_received": 10,
                "tcp_battle_final_ranking_received": 10,
                "tcp_lobby_return_received": 10,
            }

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_server = root / "fake_server.py"
            fake_server.write_text(
                _fake_metrics_server_source(latency_bucket_ms=75),
                encoding="utf-8",
            )

            result = run_concurrent_capacity_probe(
                artifact_dir=root / "artifact",
                server_argv=[
                    sys.executable,
                    str(fake_server),
                    str(root / "artifact" / "lol_game.prom"),
                ],
                host="127.0.0.1",
                tcp_port=41000,
                participants=10,
                room_size=10,
                timeout_sec=2.0,
                udp_port=41000,
                auth_token_prefix="dev-session:",
                battle_cycles=1,
                primary_latency_min_samples=1,
                primary_latency_p99_slo_ms=50.0,
                ready_timeout_sec=2.0,
                shutdown_timeout_sec=1.0,
                run_group=fake_run_group,
            )

        self.assertFalse(result["valid"])
        self.assertEqual("primary_latency_failed", result["reason"])
        self.assertEqual("FAIL", result["primary_latency_evaluation"]["result_status"])
        self.assertEqual(
            ["Primary latency"],
            result["primary_latency_evaluation"]["failed_hard_gate_categories"],
        )

    def test_runner_groups_use_unique_auth_prefixes_and_client_id_ranges(self):
        calls = []

        def fake_run_group(**kwargs):
            calls.append(kwargs)
            return {
                "valid": True,
                "reason": "",
                "rudp_move_target_sent": 40,
                "rudp_move_sent": 40,
                "rudp_attack_target_sent": 80,
                "rudp_attack_sent": 80,
                "rudp_click_loot_target_sent": 30,
                "rudp_click_loot_sent": 30,
                "rudp_click_loot_server_accept_target_sent": 28,
                "tcp_arena_gameplay_start_received": 20,
                "tcp_battle_final_ranking_received": 20,
                "tcp_lobby_return_received": 20,
            }

        with tempfile.TemporaryDirectory() as tmp:
            result = run_concurrent_runner_groups(
                artifact_dir=Path(tmp),
                host="127.0.0.1",
                tcp_port=41000,
                participants=20,
                room_size=10,
                timeout_sec=2.0,
                udp_port=41000,
                auth_token_prefix="dev-session:",
                move_duration_sec=0.2,
                move_rate_hz=10.0,
                attack_duration_sec=0.4,
                attack_rate_hz=10.0,
                battle_cycles=2,
                first_client_id=100000,
                client_id_stride=1000,
                rudp_handshake_settle_sec=0.1,
                max_workers=1,
                run_group=fake_run_group,
            )

            self.assertTrue((Path(tmp) / "runner_group_000.json").exists())
            self.assertTrue((Path(tmp) / "runner_group_001.json").exists())

        self.assertEqual(2, len(calls))
        self.assertEqual(["dev-session:G000", "dev-session:G001"], [
            call["auth_token_prefix"] for call in calls
        ])
        self.assertEqual([100000, 101000], [call["client_id_base"] for call in calls])
        self.assertEqual([10, 10], [call["participants"] for call in calls])
        self.assertEqual([0.1, 0.1], [call["rudp_handshake_settle_sec"] for call in calls])
        self.assertEqual(2, result["valid_groups"])
        self.assertEqual({"": 2}, result["group_reason_counts"])
        self.assertEqual(80, result["aggregate_runner"]["rudp_move_sent"])
        self.assertEqual(160, result["aggregate_runner"]["rudp_attack_sent"])
        self.assertEqual(56, result["aggregate_runner"]["rudp_click_loot_server_accept_target_sent"])

    def test_runner_groups_reject_client_id_stride_smaller_than_room_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, "client_id_stride"):
                run_concurrent_runner_groups(
                    artifact_dir=Path(tmp),
                    host="127.0.0.1",
                    tcp_port=41000,
                    participants=20,
                    room_size=10,
                    timeout_sec=2.0,
                    udp_port=41000,
                    auth_token_prefix="dev-session:",
                    client_id_stride=9,
                    run_group=lambda **kwargs: {"valid": True, "reason": ""},
                )


def _fake_metrics_server_source(latency_bucket_ms=20):
    metrics_text = "\n".join(
        [
            "lol_server_runtime_tick_total 1",
            "lol_rudp_move_accepted_total 2",
            "lol_rudp_attack_accepted_total 4",
            "lol_rudp_loot_claim_accepted_total 1",
            f'lol_rudp_move_receive_to_apply_latency_ms_bucket{{le="{latency_bucket_ms}"}} 2',
            "lol_rudp_move_receive_to_apply_latency_ms_count 2",
            f'lol_rudp_attack_receive_to_apply_latency_ms_bucket{{le="{latency_bucket_ms}"}} 4',
            "lol_rudp_attack_receive_to_apply_latency_ms_count 4",
            f'lol_rudp_loot_claim_receive_to_apply_latency_ms_bucket{{le="{latency_bucket_ms}"}} 1',
            "lol_rudp_loot_claim_receive_to_apply_latency_ms_count 1",
        ]
    )
    return (
        "import sys\n"
        "import time\n"
        "from pathlib import Path\n"
        f"Path(sys.argv[1]).write_text({metrics_text!r}, encoding='utf-8')\n"
        "time.sleep(60)\n"
    )


if __name__ == "__main__":
    unittest.main()
