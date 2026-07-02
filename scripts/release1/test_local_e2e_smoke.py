import json
import socket
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

from scripts.release1.local_e2e_smoke import main, run_local_e2e_smoke


class LocalE2ESmokeTests(unittest.TestCase):
    def test_smoke_runs_server_process_and_external_runner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            server_log_path = root / "server.log"
            runner_output_path = root / "runner.json"
            fake_server = _write_fake_server(root)
            port = _free_port()

            result = run_local_e2e_smoke(
                server_argv=[
                    sys.executable,
                    str(fake_server),
                    str(metrics_path),
                    str(port),
                ],
                metrics_textfile_path=metrics_path,
                server_log_path=server_log_path,
                runner_output_path=runner_output_path,
                host="127.0.0.1",
                tcp_port=port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=port,
                auth_token_prefix="dev-session:",
                start_battle=True,
            )

            self.assertTrue(result["valid"], result)
            self.assertTrue(result["server"]["ready"])
            self.assertTrue(result["server"]["stopped"])
            self.assertTrue(result["server"]["terminated"])
            self.assertFalse(result["server"]["killed"])
            self.assertTrue(result["runner"]["valid"])
            self.assertEqual(2, result["runner"]["battle_start_received"])
            self.assertEqual(2, result["runner"]["rudp_attack_sent"])
            self.assertEqual(2, result["runner"]["tcp_monster_health_snapshot_received"])
            self.assertEqual(result["runner"], json.loads(runner_output_path.read_text()))
            self.assertIn("fake e2e server ready", server_log_path.read_text())

    def test_cli_runs_smoke_and_prints_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            server_log_path = root / "server.log"
            runner_output_path = root / "runner.json"
            fake_server = _write_fake_server(root)
            port = _free_port()
            stdout = StringIO()

            with redirect_stdout(stdout):
                status = main(
                    [
                        "--metrics-textfile",
                        str(metrics_path),
                        "--server-log",
                        str(server_log_path),
                        "--runner-output",
                        str(runner_output_path),
                        "--host",
                        "127.0.0.1",
                        "--tcp-port",
                        str(port),
                        "--participants",
                        "2",
                        "--room-size",
                        "2",
                        "--timeout-sec",
                        "2",
                        "--udp-port",
                        str(port),
                        "--auth-token-prefix",
                        "dev-session:",
                        "--start-battle",
                        "--",
                        sys.executable,
                        str(fake_server),
                        str(metrics_path),
                        str(port),
                    ]
                )

            self.assertEqual(0, status)
            payload = json.loads(stdout.getvalue())
            self.assertTrue(payload["valid"], payload)
            self.assertTrue(payload["runner"]["valid"])

    def test_cli_passes_loot_smoke_to_external_runner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            server_log_path = root / "server.log"
            runner_output_path = root / "runner.json"
            fake_server = _write_fake_server(root, loot_smoke=True)
            port = _free_port()
            stdout = StringIO()

            with redirect_stdout(stdout):
                status = main(
                    [
                        "--metrics-textfile",
                        str(metrics_path),
                        "--server-log",
                        str(server_log_path),
                        "--runner-output",
                        str(runner_output_path),
                        "--host",
                        "127.0.0.1",
                        "--tcp-port",
                        str(port),
                        "--participants",
                        "2",
                        "--room-size",
                        "2",
                        "--timeout-sec",
                        "2",
                        "--udp-port",
                        str(port),
                        "--auth-token-prefix",
                        "dev-session:",
                        "--start-battle",
                        "--loot-smoke",
                        "--",
                        sys.executable,
                        str(fake_server),
                        str(metrics_path),
                        str(port),
                    ]
                )

            self.assertEqual(0, status)
            payload = json.loads(stdout.getvalue())
            self.assertTrue(payload["valid"], payload)
            self.assertTrue(payload["runner"]["valid"])
            self.assertTrue(payload["runner"]["loot_smoke"])
            self.assertEqual(2, payload["runner"]["rudp_loot_resolved_received"])

    def test_cli_passes_sustained_move_workload_to_external_runner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            server_log_path = root / "server.log"
            runner_output_path = root / "runner.json"
            fake_server = _write_fake_server(root, move_packets_per_player=3)
            port = _free_port()
            stdout = StringIO()

            with redirect_stdout(stdout):
                status = main(
                    [
                        "--metrics-textfile",
                        str(metrics_path),
                        "--server-log",
                        str(server_log_path),
                        "--runner-output",
                        str(runner_output_path),
                        "--host",
                        "127.0.0.1",
                        "--tcp-port",
                        str(port),
                        "--participants",
                        "2",
                        "--room-size",
                        "2",
                        "--timeout-sec",
                        "2",
                        "--udp-port",
                        str(port),
                        "--auth-token-prefix",
                        "dev-session:",
                        "--start-battle",
                        "--move-duration-sec",
                        "0.03",
                        "--move-rate-hz",
                        "100",
                        "--",
                        sys.executable,
                        str(fake_server),
                        str(metrics_path),
                        str(port),
                    ]
                )

            self.assertEqual(0, status)
            payload = json.loads(stdout.getvalue())
            self.assertTrue(payload["valid"], payload)
            self.assertEqual(6, payload["runner"]["rudp_move_target_sent"])
            self.assertEqual(6, payload["runner"]["rudp_move_sent"])

    def test_cli_passes_sustained_attack_workload_to_external_runner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            server_log_path = root / "server.log"
            runner_output_path = root / "runner.json"
            fake_server = _write_fake_server(
                root,
                loot_smoke=True,
                attack_packets_per_player=4,
            )
            port = _free_port()
            stdout = StringIO()

            with redirect_stdout(stdout):
                status = main(
                    [
                        "--metrics-textfile",
                        str(metrics_path),
                        "--server-log",
                        str(server_log_path),
                        "--runner-output",
                        str(runner_output_path),
                        "--host",
                        "127.0.0.1",
                        "--tcp-port",
                        str(port),
                        "--participants",
                        "2",
                        "--room-size",
                        "2",
                        "--timeout-sec",
                        "2",
                        "--udp-port",
                        str(port),
                        "--auth-token-prefix",
                        "dev-session:",
                        "--start-battle",
                        "--loot-smoke",
                        "--attack-duration-sec",
                        "0.04",
                        "--attack-rate-hz",
                        "100",
                        "--",
                        sys.executable,
                        str(fake_server),
                        str(metrics_path),
                        str(port),
                    ]
                )

            self.assertEqual(0, status)
            payload = json.loads(stdout.getvalue())
            self.assertTrue(payload["valid"], payload)
            self.assertEqual(8, payload["runner"]["rudp_attack_target_sent"])
            self.assertEqual(8, payload["runner"]["rudp_attack_sent"])

    def test_cli_passes_battle_cycles_to_external_runner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            server_log_path = root / "server.log"
            runner_output_path = root / "runner.json"
            fake_server = _write_fake_server(root, loot_smoke=True, battle_cycles=2)
            port = _free_port()
            stdout = StringIO()

            with redirect_stdout(stdout):
                status = main(
                    [
                        "--metrics-textfile",
                        str(metrics_path),
                        "--server-log",
                        str(server_log_path),
                        "--runner-output",
                        str(runner_output_path),
                        "--host",
                        "127.0.0.1",
                        "--tcp-port",
                        str(port),
                        "--participants",
                        "2",
                        "--room-size",
                        "2",
                        "--timeout-sec",
                        "2",
                        "--udp-port",
                        str(port),
                        "--auth-token-prefix",
                        "dev-session:",
                        "--start-battle",
                        "--loot-smoke",
                        "--battle-cycles",
                        "2",
                        "--",
                        sys.executable,
                        str(fake_server),
                        str(metrics_path),
                        str(port),
                    ]
                )

            self.assertEqual(0, status)
            payload = json.loads(stdout.getvalue())
            self.assertTrue(payload["valid"], payload)
            self.assertEqual(2, payload["runner"]["battle_cycles"])
            self.assertEqual(2, payload["runner"]["rooms_created"])
            self.assertEqual(4, payload["runner"]["rudp_click_loot_sent"])
            self.assertEqual(4, payload["runner"]["tcp_battle_final_ranking_received"])
            self.assertEqual(4, payload["runner"]["tcp_lobby_return_received"])

    def test_post_runner_metrics_settle_waits_before_stopping_server(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            server_log_path = root / "server.log"
            runner_output_path = root / "runner.json"
            fake_server = _write_fake_server(root, delayed_metrics_after_attack=True)
            port = _free_port()

            result = run_local_e2e_smoke(
                server_argv=[
                    sys.executable,
                    str(fake_server),
                    str(metrics_path),
                    str(port),
                ],
                metrics_textfile_path=metrics_path,
                server_log_path=server_log_path,
                runner_output_path=runner_output_path,
                host="127.0.0.1",
                tcp_port=port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=port,
                auth_token_prefix="dev-session:",
                start_battle=True,
                post_runner_metrics_settle_sec=0.2,
            )

            self.assertTrue(result["valid"], result)
            self.assertIn(
                "lol_rudp_attack_accepted_total 2",
                metrics_path.read_text(encoding="utf-8"),
            )


def _write_fake_server(
    root: Path,
    delayed_metrics_after_attack: bool = False,
    loot_smoke: bool = False,
    move_packets_per_player: int = 1,
    attack_packets_per_player: int = 4,
    battle_cycles: int = 1,
) -> Path:
    path = root / "fake_e2e_server.py"
    repo_root = Path(__file__).resolve().parents[2]
    lines = [
        "import signal",
        "import sys",
        "import time",
        "from pathlib import Path",
        "",
        f"sys.path.insert(0, {str(repo_root)!r})",
        "",
        "from scripts.release1.test_external_stress_runner import (",
        "    FakeRoomFillServer,",
        "    RUDP_TYPE_INPUT_COMMAND,",
        ")",
        "",
        "def stop(signum, frame):",
        "    sys.exit(0)",
        "",
        "signal.signal(signal.SIGTERM, stop)",
        "metrics_path = Path(sys.argv[1])",
        "port = int(sys.argv[2])",
        "delayed_metric_written = False",
        "",
        "with FakeRoomFillServer(",
        "    participants=2,",
        "    room_size=2,",
        "    enable_udp=True,",
        "    require_auth=True,",
        "    require_tcp_start=True,",
        f"    expected_rudp_move_packets_per_player={move_packets_per_player!r},",
        f"    expected_rudp_attack_packets_per_player={attack_packets_per_player!r},",
        f"    send_health_snapshot_after_attack={not loot_smoke!r},",
        f"    send_drop_after_attack_death={loot_smoke!r},",
        f"    battle_cycles={battle_cycles!r},",
        f"    send_battle_result_after_loot={loot_smoke and battle_cycles > 1!r},",
        "    port=port,",
        ") as server:",
        "    metrics_path.write_text(",
        "        \"lol_server_runtime_tick_total 1\\n\",",
        "        encoding=\"utf-8\",",
        "    )",
        "    print(\"fake e2e server ready\", flush=True)",
        "    while True:",
    ]
    if delayed_metrics_after_attack:
        lines.extend(
            [
                "        if (",
                "            not delayed_metric_written and",
                "            server.observed_rudp_packet_types.count(RUDP_TYPE_INPUT_COMMAND) >= 4",
                "        ):",
                "            time.sleep(0.05)",
                "            metrics_path.write_text(",
                "                \"lol_server_runtime_tick_total 2\\n\"",
                "                \"lol_rudp_attack_accepted_total 2\\n\",",
                "                encoding=\"utf-8\",",
                "            )",
                "            delayed_metric_written = True",
            ]
        )
    lines.append("        time.sleep(0.05)")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


if __name__ == "__main__":
    unittest.main()
