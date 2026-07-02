import json
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

from scripts.release1.server_process import main, run_server_process_lifecycle


class ServerProcessLifecycleTests(unittest.TestCase):
    def test_lifecycle_waits_for_metrics_ready_and_captures_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            log_path = root / "server.log"
            server_script = _write_fake_ready_server(root)

            result = run_server_process_lifecycle(
                argv=[sys.executable, str(server_script), str(metrics_path)],
                metrics_textfile_path=metrics_path,
                log_path=log_path,
                ready_timeout_sec=2.0,
                shutdown_timeout_sec=2.0,
                poll_interval_sec=0.01,
            )

            self.assertTrue(result["started"])
            self.assertTrue(result["ready"])
            self.assertTrue(result["stopped"])
            self.assertTrue(result["terminated"])
            self.assertFalse(result["killed"])
            self.assertEqual("", result["reason"])
            self.assertIn("fake server started", log_path.read_text(encoding="utf-8"))

    def test_lifecycle_reports_process_exit_before_readiness(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            log_path = root / "server.log"
            server_script = _write_fake_exiting_server(root)

            result = run_server_process_lifecycle(
                argv=[sys.executable, str(server_script)],
                metrics_textfile_path=metrics_path,
                log_path=log_path,
                ready_timeout_sec=2.0,
                shutdown_timeout_sec=2.0,
                poll_interval_sec=0.01,
            )

            self.assertTrue(result["started"])
            self.assertFalse(result["ready"])
            self.assertTrue(result["stopped"])
            self.assertFalse(result["terminated"])
            self.assertFalse(result["killed"])
            self.assertEqual(3, result["return_code"])
            self.assertEqual("process_exited_before_ready", result["reason"])
            self.assertIn("exiting before metrics", log_path.read_text(encoding="utf-8"))

    def test_cli_runs_lifecycle_and_prints_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            metrics_path = root / "lol_game.prom"
            log_path = root / "server.log"
            server_script = _write_fake_ready_server(root)
            stdout = StringIO()

            with redirect_stdout(stdout):
                status = main(
                    [
                        "--metrics-textfile",
                        str(metrics_path),
                        "--log",
                        str(log_path),
                        "--ready-timeout-sec",
                        "2",
                        "--shutdown-timeout-sec",
                        "2",
                        "--",
                        sys.executable,
                        str(server_script),
                        str(metrics_path),
                    ]
                )

            self.assertEqual(0, status)
            payload = json.loads(stdout.getvalue())
            self.assertTrue(payload["ready"])
            self.assertTrue(payload["stopped"])
            self.assertEqual(str(metrics_path), payload["metrics_textfile_path"])
            self.assertEqual(str(log_path), payload["log_path"])


def _write_fake_ready_server(root: Path) -> Path:
    path = root / "fake_ready_server.py"
    path.write_text(
        textwrap.dedent(
            """
            import signal
            import sys
            import time
            from pathlib import Path

            def stop(signum, frame):
                sys.exit(0)

            signal.signal(signal.SIGTERM, stop)
            metrics_path = Path(sys.argv[1])
            print("fake server started", flush=True)
            time.sleep(0.05)
            metrics_path.write_text("lol_server_runtime_tick_total 1\\n", encoding="utf-8")
            while True:
                time.sleep(0.05)
            """
        ).lstrip(),
        encoding="utf-8",
    )
    return path


def _write_fake_exiting_server(root: Path) -> Path:
    path = root / "fake_exiting_server.py"
    path.write_text(
        'print("exiting before metrics", flush=True)\nraise SystemExit(3)\n',
        encoding="utf-8",
    )
    return path


if __name__ == "__main__":
    unittest.main()
