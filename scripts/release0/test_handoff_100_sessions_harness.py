import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from scripts.release0.handoff_foundation_harness import HarnessFailure
from scripts.release0.handoff_100_sessions_harness import (
    Report,
    parse_args,
    redact_queued_admission,
    run,
)


class ReportShapeTests(unittest.TestCase):
    def test_report_uses_100_session_run_name_and_redacts_tokens(self):
        report = Report(capacity=100, players=101)
        report.start_step(
            "token_string_redaction",
            {
                "path": "/api/release0/admission/queue/raw-queue-token",
                "message": "gameSessionToken=raw-game-session-token",
            },
        )
        report.fail_step("token_string_redaction", "queueToken=raw-queue-token")
        report.player_results.append(
            {
                "label": "player101",
                "accountId": 101,
                "nickname": "hf101",
                "admission": {
                    "status": "Queued",
                    "queueToken": "raw-queue-token",
                    "gameSessionToken": "raw-game-session-token",
                    "queueTokenReceived": True,
                    "gameSessionTokenReceived": False,
                    "cxxConnectAttemptedWhileQueued": False,
                },
            }
        )

        serialized = json.dumps(report.to_json())

        self.assertIn('"runName": "handoff-100-sessions"', serialized)
        self.assertIn('"capacity": 100', serialized)
        self.assertIn('"players": 101', serialized)
        self.assertIn("queueTokenReceived", serialized)
        self.assertIn("gameSessionTokenReceived", serialized)
        self.assertNotIn("raw-queue-token", serialized)
        self.assertNotIn("raw-game-session-token", serialized)
        self.assertNotIn('"queueToken"', serialized)
        self.assertNotIn('"gameSessionToken"', serialized)

    def test_redacts_queued_admission_and_records_no_cxx_connect(self):
        redacted = redact_queued_admission(
            {
                "status": "Queued",
                "position": 1,
                "queueToken": "raw-queue-token",
            },
            cxx_connect_attempted=False,
        )

        self.assertEqual(redacted["status"], "Queued")
        self.assertEqual(redacted["position"], 1)
        self.assertTrue(redacted["queueTokenReceived"])
        self.assertFalse(redacted["gameSessionTokenReceived"])
        self.assertFalse(redacted["cxxConnectAttemptedWhileQueued"])
        self.assertNotIn("queueToken", redacted)


class ParseArgsTests(unittest.TestCase):
    def test_script_path_help_is_invokable_from_repo_root(self):
        repo_root = Path(__file__).resolve().parents[2]

        result = subprocess.run(
            [
                sys.executable,
                "scripts/release0/handoff_100_sessions_harness.py",
                "--help",
            ],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Run Release 0 100-session handoff evidence.", result.stdout)

    def test_defaults_to_full_100_101_shape(self):
        args = parse_args(["--internal-token", "secret"])

        self.assertEqual(args.capacity, 100)
        self.assertEqual(args.players, 101)
        self.assertEqual(args.report, "artifacts/release0/handoff_100_sessions_report.json")

    def test_rejects_non_100_101_shape(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parse_args(["--internal-token", "secret", "--capacity", "2", "--players", "3"])

    def test_requires_internal_token(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parse_args([])


class RunnerFlowTests(unittest.TestCase):
    def _runner_args(self, report_path):
        return SimpleNamespace(
            capacity=100,
            players=101,
            meta_base_url="http://127.0.0.1:8081",
            internal_token="secret",
            timeout_seconds=1.0,
            test_account_header="X-Release0-Test-Account",
            game_host="127.0.0.1",
            game_tcp_port=40000,
            nickname_prefix="h100",
            report=report_path,
        )

    def test_sequential_100_clients_queue_101_direct_release_and_promote(self):
        close_events = []
        authenticate_tokens = []
        connect_probe_count = 0
        connect_probe_auth_counts = []
        snapshots = {}

        def capture_snapshot(step):
            snapshots[step] = {
                "closed": list(close_events),
                "auth": list(authenticate_tokens),
            }

        class FakeAuthenticatedClient:
            def __init__(self, label, session_id):
                self.label = label
                self.session_id = session_id
                self.initial_room_list = []

            def to_report(self):
                return {
                    "sessionId": self.session_id,
                    "initialRoomList": self.initial_room_list,
                }

            def close(self):
                close_events.append(self.label)

        class FakeTcpProbe:
            def __enter__(self):
                nonlocal connect_probe_count
                connect_probe_count += 1
                return self

            def __exit__(self, _exc_type, _exc, _traceback):
                return False

        class FakeHttpClient:
            def __init__(self):
                self.calls = []

            def load_csrf(self, step):
                capture_snapshot(step)
                return {
                    "headerName": "X-CSRF-TOKEN",
                    "parameterName": "_csrf",
                    "token": "csrf-token-123",
                }

            def send_empty(self, step, method, path, internal=False, account_id=None):
                self.calls.append(
                    {
                        "step": step,
                        "method": method,
                        "path": path,
                        "body": None,
                        "internal": internal,
                        "account_id": account_id,
                    }
                )
                if step == "test_identity_ready":
                    return 204
                if step == "direct_release_player_1":
                    capture_snapshot(step)
                    self.released_path = path
                    self.released_method = method
                    self.released_internal = internal
                    self.released_account_id = account_id
                    return 204
                raise AssertionError(f"unexpected empty step {step}")

            def send_json(
                self,
                step,
                method,
                path,
                body=None,
                internal=False,
                account_id=None,
            ):
                self.calls.append(
                    {
                        "step": step,
                        "method": method,
                        "path": path,
                        "body": body,
                        "internal": internal,
                        "account_id": account_id,
                    }
                )
                if step == "meta_health":
                    return {"status": "UP"}
                if step == "create_test_identities":
                    self.identities = [
                        {"accountId": index, "nickname": f"h100{index}"}
                        for index in range(1, 102)
                    ]
                    return {"identities": self.identities}
                if step.startswith("admit_player_"):
                    player_number = int(step.removeprefix("admit_player_"))
                    return self._admitted(f"token-player{player_number}")
                if step == "queue_player_101":
                    capture_snapshot(step)
                    return {"status": "Queued", "position": 1, "queueToken": "raw-queue-token"}
                if step == "promote_player_101":
                    capture_snapshot(step)
                    return self._admitted("token-player101")
                raise AssertionError(f"unexpected json step {step}")

            def _admitted(self, token):
                return {
                    "status": "Admitted",
                    "gameSessionToken": token,
                    "reservationExpiresAt": 999999,
                    "gameServerEndpoint": {
                        "host": "127.0.0.1",
                        "tcpPort": 40000,
                        "rudpPort": 40000,
                    },
                }

        def fake_authenticate(_host, _tcp_port, token, _timeout_seconds):
            authenticate_tokens.append(token)
            player_number = int(token.removeprefix("token-player"))
            return FakeAuthenticatedClient(f"player{player_number}", 1000 + player_number)

        def fake_connect(_address, timeout=None):
            connect_probe_auth_counts.append(len(authenticate_tokens))
            return FakeTcpProbe()

        fake_http = FakeHttpClient()
        with tempfile.NamedTemporaryFile(delete=True) as report_file:
            args = self._runner_args(report_file.name)

            exit_code = run(
                args,
                http_client=fake_http,
                authenticate=fake_authenticate,
                connect=fake_connect,
            )
            report_file.seek(0)
            report_payload = report_file.read().decode("utf-8")

        report = json.loads(report_payload)
        self.assertEqual(exit_code, 0)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["capacity"], 100)
        self.assertEqual(report["players"], 101)
        self.assertEqual(len(report["playerResults"]), 101)
        self.assertEqual(len(authenticate_tokens), 101)
        self.assertEqual(authenticate_tokens[:100], [f"token-player{index}" for index in range(1, 101)])
        self.assertEqual(authenticate_tokens[100], "token-player101")
        self.assertEqual(connect_probe_count, 1)
        self.assertEqual(connect_probe_auth_counts, [0])
        self.assertEqual(snapshots["queue_player_101"]["closed"], [])
        self.assertEqual(snapshots["queue_player_101"]["auth"], [f"token-player{index}" for index in range(1, 101)])
        self.assertEqual(snapshots["direct_release_player_1"]["closed"], [])
        self.assertEqual(snapshots["direct_release_player_1"]["auth"], [f"token-player{index}" for index in range(1, 101)])
        self.assertEqual(
            fake_http.released_path,
            "/internal/release0/test-admission-identities/1/release-active-session",
        )
        self.assertEqual(fake_http.released_method, "POST")
        self.assertTrue(fake_http.released_internal)
        self.assertIsNone(fake_http.released_account_id)
        self.assertIn("player1", snapshots["promote_player_101"]["closed"])
        self.assertNotIn("player2", snapshots["promote_player_101"]["closed"])
        self.assertEqual(snapshots["promote_player_101"]["auth"], [f"token-player{index}" for index in range(1, 101)])
        self.assertNotIn("raw-queue-token", authenticate_tokens)
        self.assertNotIn("raw-queue-token", report_payload)
        self.assertNotIn("csrf-token-123", report_payload)
        queued_result = report["playerResults"][100]
        self.assertEqual(queued_result["label"], "player101")
        self.assertEqual(queued_result["queuedAdmission"]["position"], 1)
        self.assertFalse(queued_result["queuedAdmission"]["cxxConnectAttemptedWhileQueued"])
        self.assertEqual(queued_result["cxx"]["sessionId"], 1101)
        self.assertEqual(close_events[0], "player1")
        self.assertEqual(
            Counter(close_events),
            Counter(f"player{index}" for index in range(1, 102)),
        )
        self.assertEqual(
            fake_http.calls[0],
            {
                "step": "meta_health",
                "method": "GET",
                "path": "/actuator/health",
                "body": None,
                "internal": False,
                "account_id": None,
            },
        )
        self.assertEqual(
            [call["step"] for call in fake_http.calls[:3]],
            ["meta_health", "test_identity_ready", "create_test_identities"],
        )
        identity_ready_call = next(call for call in fake_http.calls if call["step"] == "test_identity_ready")
        self.assertEqual(
            identity_ready_call,
            {
                "step": "test_identity_ready",
                "method": "GET",
                "path": "/internal/release0/test-admission-identities/ready",
                "body": None,
                "internal": True,
                "account_id": None,
            },
        )
        create_identities_call = next(call for call in fake_http.calls if call["step"] == "create_test_identities")
        self.assertEqual(
            create_identities_call,
            {
                "step": "create_test_identities",
                "method": "POST",
                "path": "/internal/release0/test-admission-identities",
                "body": {"count": 101, "nicknamePrefix": "h100"},
                "internal": True,
                "account_id": None,
            },
        )
        admit_calls = [call for call in fake_http.calls if call["step"].startswith("admit_player_")]
        self.assertEqual(len(admit_calls), 100)
        self.assertEqual([call["account_id"] for call in admit_calls], list(range(1, 101)))
        self.assertTrue(
            all(
                call["method"] == "POST"
                and call["path"] == "/api/release0/admission/enter"
                and call["body"] is None
                and not call["internal"]
                for call in admit_calls
            )
        )
        queue_call = next(call for call in fake_http.calls if call["step"] == "queue_player_101")
        self.assertEqual(
            queue_call,
            {
                "step": "queue_player_101",
                "method": "POST",
                "path": "/api/release0/admission/enter",
                "body": None,
                "internal": False,
                "account_id": 101,
            },
        )
        promote_call = next(call for call in fake_http.calls if call["step"] == "promote_player_101")
        self.assertEqual(promote_call["method"], "GET")
        self.assertEqual(promote_call["path"], "/api/release0/admission/queue/raw-queue-token")
        self.assertEqual(promote_call["account_id"], 101)

    def test_authentication_failure_closes_open_clients_and_writes_fail_report(self):
        close_events = []

        class FakeAuthenticatedClient:
            def __init__(self, label):
                self.label = label

            def to_report(self):
                return {"sessionId": int(self.label.removeprefix("player")), "initialRoomList": []}

            def close(self):
                close_events.append(self.label)

        class FakeTcpProbe:
            def __enter__(self):
                return self

            def __exit__(self, _exc_type, _exc, _traceback):
                return False

        class FakeHttpClient:
            def load_csrf(self, _step):
                return {"headerName": "X-CSRF-TOKEN", "parameterName": "_csrf", "token": "csrf-token"}

            def send_empty(self, _step, _method, _path, internal=False, account_id=None):
                return 204

            def send_json(self, step, _method, _path, body=None, internal=False, account_id=None):
                if step == "meta_health":
                    return {"status": "UP"}
                if step == "create_test_identities":
                    return {
                        "identities": [
                            {"accountId": index, "nickname": f"h100{index}"}
                            for index in range(1, 102)
                        ]
                    }
                if step.startswith("admit_player_"):
                    player_number = int(step.removeprefix("admit_player_"))
                    return {
                        "status": "Admitted",
                        "gameSessionToken": f"token-player{player_number}",
                        "gameServerEndpoint": {"host": "127.0.0.1", "tcpPort": 40000},
                    }
                raise AssertionError(f"unexpected json step {step}")

        def fake_authenticate(_host, _tcp_port, token, _timeout_seconds):
            player_number = int(token.removeprefix("token-player"))
            if player_number == 3:
                raise HarnessFailure("cxx_auth", "auth failed for gameSessionToken=raw-game-session-token")
            return FakeAuthenticatedClient(f"player{player_number}")

        with tempfile.NamedTemporaryFile(delete=True) as report_file:
            exit_code = run(
                self._runner_args(report_file.name),
                http_client=FakeHttpClient(),
                authenticate=fake_authenticate,
                connect=lambda _address, timeout=None: FakeTcpProbe(),
            )
            report_file.seek(0)
            report_payload = report_file.read().decode("utf-8")

        report = json.loads(report_payload)
        self.assertEqual(exit_code, 1)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["failedStep"], "admit_player_3")
        self.assertNotIn("raw-game-session-token", report_payload)
        self.assertEqual(close_events, ["player2", "player1"])

    def test_report_write_failure_still_closes_open_clients(self):
        close_events = []

        class FakeAuthenticatedClient:
            def __init__(self, label):
                self.label = label

            def to_report(self):
                return {"sessionId": 1, "initialRoomList": []}

            def close(self):
                close_events.append(self.label)

        class FakeTcpProbe:
            def __enter__(self):
                return self

            def __exit__(self, _exc_type, _exc, _traceback):
                return False

        class FakeHttpClient:
            def load_csrf(self, _step):
                return {"headerName": "X-CSRF-TOKEN", "parameterName": "_csrf", "token": "csrf-token"}

            def send_empty(self, _step, _method, _path, internal=False, account_id=None):
                return 204

            def send_json(self, step, _method, _path, body=None, internal=False, account_id=None):
                if step == "meta_health":
                    return {"status": "UP"}
                if step == "create_test_identities":
                    return {
                        "identities": [
                            {"accountId": index, "nickname": f"h100{index}"}
                            for index in range(1, 102)
                        ]
                    }
                if step.startswith("admit_player_"):
                    player_number = int(step.removeprefix("admit_player_"))
                    return {
                        "status": "Admitted",
                        "gameSessionToken": f"token-player{player_number}",
                        "gameServerEndpoint": {"host": "127.0.0.1", "tcpPort": 40000},
                    }
                raise AssertionError(f"unexpected json step {step}")

        def fake_authenticate(_host, _tcp_port, token, _timeout_seconds):
            player_number = int(token.removeprefix("token-player"))
            if player_number == 2:
                raise HarnessFailure("cxx_auth", "auth failed")
            return FakeAuthenticatedClient(f"player{player_number}")

        with mock.patch(
            "scripts.release0.handoff_100_sessions_harness.write_report",
            side_effect=OSError("disk full"),
        ):
            with self.assertRaises(OSError):
                run(
                    self._runner_args("unused-report.json"),
                    http_client=FakeHttpClient(),
                    authenticate=fake_authenticate,
                    connect=lambda _address, timeout=None: FakeTcpProbe(),
                )

        self.assertEqual(close_events, ["player1"])

    def test_unexpected_exception_closes_report_as_fail(self):
        close_events = []

        class FakeAuthenticatedClient:
            def __init__(self, label):
                self.label = label

            def to_report(self):
                return {"sessionId": int(self.label.removeprefix("player")), "initialRoomList": []}

            def close(self):
                close_events.append(self.label)

        class FakeTcpProbe:
            def __enter__(self):
                return self

            def __exit__(self, _exc_type, _exc, _traceback):
                return False

        class FakeHttpClient:
            def load_csrf(self, _step):
                return {"headerName": "X-CSRF-TOKEN", "parameterName": "_csrf", "token": "csrf-token"}

            def send_empty(self, _step, _method, _path, internal=False, account_id=None):
                return 204

            def send_json(self, step, _method, _path, body=None, internal=False, account_id=None):
                if step == "meta_health":
                    return {"status": "UP"}
                if step == "create_test_identities":
                    return {
                        "identities": [
                            {"accountId": index, "nickname": f"h100{index}"}
                            for index in range(1, 102)
                        ]
                    }
                if step.startswith("admit_player_"):
                    player_number = int(step.removeprefix("admit_player_"))
                    return {
                        "status": "Admitted",
                        "gameSessionToken": f"token-player{player_number}",
                        "gameServerEndpoint": {"host": "127.0.0.1", "tcpPort": 40000},
                    }
                raise AssertionError(f"unexpected json step {step}")

        def fake_authenticate(_host, _tcp_port, token, _timeout_seconds):
            player_number = int(token.removeprefix("token-player"))
            if player_number == 2:
                raise RuntimeError("Authorization: Bearer raw-internal-token")
            return FakeAuthenticatedClient(f"player{player_number}")

        with tempfile.NamedTemporaryFile(delete=True) as report_file:
            exit_code = run(
                self._runner_args(report_file.name),
                http_client=FakeHttpClient(),
                authenticate=fake_authenticate,
                connect=lambda _address, timeout=None: FakeTcpProbe(),
            )
            report_file.seek(0)
            report_payload = report_file.read().decode("utf-8")

        report = json.loads(report_payload)
        self.assertEqual(exit_code, 1)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["failedStep"], "admit_player_2")
        self.assertIn("unexpected RuntimeError", report_payload)
        self.assertNotIn("raw-internal-token", report_payload)
        self.assertNotIn("Authorization", report_payload)
        self.assertEqual(close_events, ["player1"])


if __name__ == "__main__":
    unittest.main()
