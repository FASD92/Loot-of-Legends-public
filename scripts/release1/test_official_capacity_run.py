import json
import signal
import socket
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from scripts.release1.capacity_report import load_json
from scripts.release1.official_capacity_run import run_official_capacity_smoke


class OfficialCapacityRunTests(unittest.TestCase):
    def test_smoke_runner_writes_bundle_with_gate_evaluations(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifact_dir = root / "bundle"
            fake_server = _write_fake_server(
                root,
                move_packets_per_player=3,
                attack_packets_per_player=4,
            )
            port = _free_port()
            metrics_path = artifact_dir / "lol_game.prom"

            result = run_official_capacity_smoke(
                run_id="20260701-010203-smoke-deadbee",
                artifact_dir=artifact_dir,
                server_argv=[
                    sys.executable,
                    str(fake_server),
                    str(metrics_path),
                    str(port),
                ],
                host="127.0.0.1",
                tcp_port=port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=port,
                auth_token_prefix="dev-session:",
                official_window_sec=0.0,
                move_duration_sec=0.03,
                move_rate_hz=100.0,
                attack_duration_sec=0.04,
                attack_rate_hz=100.0,
                post_runner_metrics_settle_sec=0.05,
                shutdown_timeout_sec=2.0,
            )

            self.assertTrue(result["valid"], result)
            self.assertEqual(
                "PASS",
                load_json(artifact_dir / "evaluation.json")["result_status"],
            )
            self.assertEqual(
                "PASS",
                load_json(artifact_dir / "runner_delivery_evaluation.json")[
                    "result_status"
                ],
            )
            runner = load_json(artifact_dir / "runner.json")
            self.assertEqual(6, runner["rudp_move_target_sent"])
            self.assertEqual(6, runner["rudp_move_sent"])
            accepted = load_json(
                artifact_dir / "server_accepted_delivery_evaluation.json"
            )
            self.assertEqual("PASS", accepted["result_status"])
            self.assertEqual(
                1.0,
                accepted["op_results"]["Loot claim"]["accepted_ratio"],
            )
            manifest = load_json(artifact_dir / "manifest.json")
            self.assertIn("runner.json", manifest["artifacts"])
            self.assertIn("lol_game.prom", manifest["artifacts"])
            self.assertIn("official_window.json", manifest["artifacts"])
            self.assertIn(
                "server_accepted_delivery_evaluation.json",
                manifest["artifacts"],
            )
            self.assertTrue(load_json(artifact_dir / "official_window.json")["completed"])

    def test_unmet_official_window_evaluates_to_aborted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifact_dir = root / "bundle"
            fake_server = _write_fake_server(root)
            port = _free_port()
            metrics_path = artifact_dir / "lol_game.prom"

            result = run_official_capacity_smoke(
                run_id="20260701-010204-window-deadbee",
                artifact_dir=artifact_dir,
                server_argv=[
                    sys.executable,
                    str(fake_server),
                    str(metrics_path),
                    str(port),
                ],
                host="127.0.0.1",
                tcp_port=port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=port,
                auth_token_prefix="dev-session:",
                official_window_sec=999.0,
                post_runner_metrics_settle_sec=0.01,
                shutdown_timeout_sec=2.0,
            )

            self.assertFalse(result["valid"], result)
            self.assertEqual(
                "ABORTED",
                load_json(artifact_dir / "evaluation.json")["result_status"],
            )
            official_window = load_json(artifact_dir / "official_window.json")
            self.assertFalse(official_window["completed"])
            self.assertEqual(999.0, official_window["required_seconds"])

    def test_smoke_runner_passes_battle_cycles_to_local_runner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifact_dir = root / "bundle"
            fake_server = _write_fake_server(root, battle_cycles=2)
            port = _free_port()
            metrics_path = artifact_dir / "lol_game.prom"

            result = run_official_capacity_smoke(
                run_id="20260701-010205-cycles-deadbee",
                artifact_dir=artifact_dir,
                server_argv=[
                    sys.executable,
                    str(fake_server),
                    str(metrics_path),
                    str(port),
                ],
                host="127.0.0.1",
                tcp_port=port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=port,
                auth_token_prefix="dev-session:",
                official_window_sec=0.0,
                battle_cycles=2,
                post_runner_metrics_settle_sec=0.05,
                shutdown_timeout_sec=2.0,
            )

            self.assertTrue(result["valid"], result)
            runner = load_json(artifact_dir / "runner.json")
            self.assertEqual(2, runner["battle_cycles"])
            self.assertEqual(2, runner["rooms_created"])
            self.assertEqual(4, runner["tcp_battle_final_ranking_received"])
            accepted = load_json(
                artifact_dir / "server_accepted_delivery_evaluation.json"
            )
            self.assertEqual("PASS", accepted["result_status"])


def _write_fake_server(
    root: Path,
    move_packets_per_player: int = 1,
    attack_packets_per_player: int = 4,
    battle_cycles: int = 1,
) -> Path:
    path = root / "fake_official_capacity_server.py"
    repo_root = Path(__file__).resolve().parents[2]
    path.write_text(
        textwrap.dedent(
            f"""
            import signal
            import sys
            import time
            from pathlib import Path

            sys.path.insert(0, {str(repo_root)!r})

            from scripts.release1.test_external_stress_runner import FakeRoomFillServer

            def stop(signum, frame):
                sys.exit(0)

            signal.signal(signal.SIGTERM, stop)
            metrics_path = Path(sys.argv[1])
            port = int(sys.argv[2])
            move_packets_per_player = {move_packets_per_player!r}
            attack_packets_per_player = {attack_packets_per_player!r}
            battle_cycles = {battle_cycles!r}

            with FakeRoomFillServer(
                participants=2,
                room_size=2,
                enable_udp=True,
                require_auth=True,
                require_tcp_start=True,
                expected_rudp_move_packets_per_player=move_packets_per_player,
                expected_rudp_attack_packets_per_player=attack_packets_per_player,
                send_health_snapshot_after_attack=False,
                send_drop_after_attack_death=True,
                battle_cycles=battle_cycles,
                send_battle_result_after_loot=battle_cycles > 1,
                port=port,
            ):
                metrics_path.parent.mkdir(parents=True, exist_ok=True)
                metrics_path.write_text(
                    "lol_server_runtime_tick_total 1\\n"
                    f"lol_rudp_move_accepted_total {{2 * move_packets_per_player * battle_cycles}}\\n"
                    f"lol_rudp_attack_accepted_total {{2 * attack_packets_per_player * battle_cycles}}\\n"
                    f"lol_rudp_loot_claim_accepted_total {{2 * battle_cycles}}\\n",
                    encoding="utf-8",
                )
                print("fake official capacity server ready", flush=True)
                while True:
                    time.sleep(0.05)
            """
        ).lstrip(),
        encoding="utf-8",
    )
    return path


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


if __name__ == "__main__":
    unittest.main()
