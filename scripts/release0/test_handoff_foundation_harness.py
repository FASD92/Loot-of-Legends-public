import io
import json
import struct
import tempfile
import unittest
from email.message import Message
from http.cookiejar import CookieJar
from types import SimpleNamespace
from urllib import error, request

from scripts.release0.handoff_foundation_harness import (
    HarnessFailure,
    HttpClient,
    Report,
    TcpProtocol,
    redact_admission,
    run,
)


class FakeResponse:
    def __init__(self, body, status=200):
        if isinstance(body, str):
            self._body = body.encode("utf-8")
        else:
            self._body = json.dumps(body).encode("utf-8")
        self.status = status

    def __enter__(self):
        return self

    def __exit__(self, _exc_type, _exc, _traceback):
        return False

    def read(self):
        return self._body


class FakeOpener:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    def open(self, req, timeout=None):
        self.requests.append((req, timeout))
        response = self.responses.pop(0)
        if isinstance(response, BaseException):
            raise response
        return response


def make_urllib_response(req, body, set_cookie=None):
    headers = Message()
    headers.add_header("Content-Type", "application/json")
    if set_cookie is not None:
        headers.add_header("Set-Cookie", set_cookie)
    response = request.addinfourl(
        io.BytesIO(json.dumps(body).encode("utf-8")),
        headers,
        req.full_url,
        200,
    )
    response.msg = "OK"
    return response


class CookieRoundTripHandler(request.BaseHandler):
    handler_order = 100

    def __init__(self):
        self.requests = []

    def http_open(self, req):
        self.requests.append(req)
        if len(self.requests) == 1:
            return make_urllib_response(
                req,
                {
                    "headerName": "X-CSRF-TOKEN",
                    "parameterName": "_csrf",
                    "token": "csrf-token-123",
                },
                set_cookie="JSESSIONID=session-123; Path=/",
            )
        return make_urllib_response(req, {"status": "ok"})


class TcpProtocolTests(unittest.TestCase):
    def test_authenticate_game_session_packet_shape(self):
        packet = TcpProtocol.serialize_authenticate_game_session("token-123")

        self.assertEqual(packet[:4], struct.pack(">HH", 15, 0x0003))
        self.assertEqual(packet[4:6], struct.pack(">H", 9))
        self.assertEqual(packet[6:], b"token-123")

    def test_parse_welcome_requires_type_and_nonzero_session(self):
        packet = struct.pack(">HHQ", 12, 0x0001, 123)

        self.assertEqual(TcpProtocol.parse_welcome(packet), 123)

        with self.assertRaisesRegex(HarnessFailure, "invalid Welcome packet"):
            TcpProtocol.parse_welcome(struct.pack(">HHQ", 12, 0x0002, 123))

        with self.assertRaisesRegex(HarnessFailure, "invalid Welcome packet"):
            TcpProtocol.parse_welcome(struct.pack(">HHQ", 12, 0x0001, 0))

    def test_parse_room_list_snapshot_with_status(self):
        packet = (
            struct.pack(">HHH", 15, 0x0107, 1)
            + struct.pack(">IHHB", 77, 2, 10, 0)
        )

        rooms = TcpProtocol.parse_room_list_snapshot(packet)

        self.assertEqual(
            rooms,
            [{"roomId": 77, "playerCount": 2, "maxPlayers": 10, "roomStatus": "OPEN"}],
        )

    def test_parse_room_list_snapshot_rejects_unknown_status(self):
        packet = (
            struct.pack(">HHH", 15, 0x0107, 1)
            + struct.pack(">IHHB", 77, 2, 10, 9)
        )

        with self.assertRaisesRegex(HarnessFailure, "unknown room status"):
            TcpProtocol.parse_room_list_snapshot(packet)


class ReportTests(unittest.TestCase):
    def test_redacts_admission_token(self):
        admission = {
            "status": "Admitted",
            "gameSessionToken": "secret-token",
            "reservationExpiresAt": 1234,
            "gameServerEndpoint": {"host": "127.0.0.1", "tcpPort": 40000, "rudpPort": 40000},
        }

        redacted = redact_admission(admission)

        self.assertNotIn("secret-token", json.dumps(redacted))
        self.assertNotIn("gameSessionToken", redacted)
        self.assertEqual(redacted["gameSessionTokenReceived"], True)

    def test_report_marks_first_failure_and_stops(self):
        report = Report(capacity=2, players=3)
        report.start_step("meta_health")
        report.fail_step("meta_health", "connection refused")

        self.assertEqual(report.status, "FAIL")
        self.assertEqual(report.failed_step, "meta_health")
        self.assertEqual(report.steps[0]["status"], "FAIL")
        self.assertEqual(report.steps[0]["error"], "connection refused")

    def test_report_serialization_does_not_include_admission_token(self):
        report = Report(capacity=2, players=3)
        report.player_results.append(
            {
                "label": "player1",
                "admission": redact_admission(
                    {
                        "status": "Admitted",
                        "gameSessionToken": "secret-token",
                        "gameServerEndpoint": {
                            "host": "127.0.0.1",
                            "tcpPort": 40000,
                            "rudpPort": 40000,
                        },
                    }
                ),
            }
        )

        serialized = json.dumps(report.to_json())

        self.assertNotIn("secret-token", serialized)
        self.assertNotIn('"gameSessionToken":', serialized)


class HttpClientTests(unittest.TestCase):
    def test_builds_json_request_with_internal_token(self):
        client = HttpClient("http://127.0.0.1:8081", "secret", timeout_seconds=1.0)

        req = client.build_request(
            "POST",
            "/internal/release0/test-admission-identities",
            {"count": 3, "nicknamePrefix": "hf"},
            internal=True,
            account_id=None,
        )

        self.assertEqual(
            req.full_url,
            "http://127.0.0.1:8081/internal/release0/test-admission-identities",
        )
        self.assertEqual(req.get_header("X-internal-token"), "secret")
        self.assertEqual(req.get_header("Content-type"), "application/json")
        self.assertEqual(json.loads(req.data.decode("utf-8"))["count"], 3)

    def test_builds_public_admission_request_with_test_account_header(self):
        client = HttpClient("http://127.0.0.1:8081", "secret", timeout_seconds=1.0)

        req = client.build_request(
            "POST",
            "/api/release0/admission/enter",
            None,
            internal=False,
            account_id=77,
        )

        self.assertEqual(req.get_header("X-release0-test-account"), "77")

    def test_loads_csrf_token_and_applies_it_to_public_unsafe_request(self):
        opener = FakeOpener(
            [
                FakeResponse(
                    {
                        "headerName": "X-CSRF-TOKEN",
                        "parameterName": "_csrf",
                        "token": "csrf-token-123",
                    }
                )
            ]
        )
        client = HttpClient(
            "http://127.0.0.1:8081",
            "secret",
            timeout_seconds=1.0,
            opener=opener,
        )

        csrf = client.load_csrf("csrf_token")
        req = client.build_request(
            "POST",
            "/api/release0/admission/enter",
            None,
            internal=False,
            account_id=77,
        )

        self.assertEqual(csrf["headerName"], "X-CSRF-TOKEN")
        self.assertEqual(
            opener.requests[0][0].full_url,
            "http://127.0.0.1:8081/api/release0/auth/csrf",
        )
        self.assertEqual(req.get_header("X-csrf-token"), "csrf-token-123")

    def test_keeps_csrf_session_cookie_for_public_unsafe_request(self):
        handler = CookieRoundTripHandler()
        opener = request.build_opener(request.HTTPCookieProcessor(CookieJar()), handler)
        client = HttpClient(
            "http://127.0.0.1:8081",
            "secret",
            timeout_seconds=1.0,
            opener=opener,
        )

        client.load_csrf("csrf_token")
        client.send_json(
            "admit_player_1",
            "POST",
            "/api/release0/admission/enter",
            account_id=77,
        )

        self.assertEqual(handler.requests[1].get_header("Cookie"), "JSESSIONID=session-123")
        self.assertEqual(handler.requests[1].get_header("X-csrf-token"), "csrf-token-123")

    def test_does_not_apply_csrf_token_to_internal_request(self):
        client = HttpClient("http://127.0.0.1:8081", "secret", timeout_seconds=1.0)
        client.csrf_header_name = "X-CSRF-TOKEN"
        client.csrf_token = "csrf-token-123"

        req = client.build_request(
            "POST",
            "/internal/release0/test-admission-identities",
            {"count": 3},
            internal=True,
            account_id=None,
        )

        self.assertIsNone(req.get_header("X-csrf-token"))

    def test_redacts_queue_token_from_http_error_message(self):
        token = "raw-queue-token"
        failure = error.HTTPError(
            url="",
            code=500,
            msg="Internal Server Error",
            hdrs=None,
            fp=io.BytesIO(f"failed /api/release0/admission/queue/{token}".encode("utf-8")),
        )
        client = HttpClient(
            "http://127.0.0.1:8081",
            "secret",
            timeout_seconds=1.0,
            opener=FakeOpener([failure]),
        )

        with self.assertRaises(HarnessFailure) as raised:
            client.send_json(
                "promote_player_3",
                "GET",
                f"/api/release0/admission/queue/{token}",
                account_id=77,
            )

        self.assertNotIn(token, raised.exception.message)
        self.assertIn("/api/release0/admission/queue/<redacted>", raised.exception.message)

    def test_redacts_loaded_csrf_token_from_http_error_message(self):
        csrf_token = "csrf-token-123"
        failure = error.HTTPError(
            url="",
            code=403,
            msg="Forbidden",
            hdrs=None,
            fp=io.BytesIO(f"debug echoed token {csrf_token}".encode("utf-8")),
        )
        client = HttpClient(
            "http://127.0.0.1:8081",
            "secret",
            timeout_seconds=1.0,
            opener=FakeOpener(
                [
                    FakeResponse(
                        {
                            "headerName": "X-CSRF-TOKEN",
                            "parameterName": "_csrf",
                            "token": csrf_token,
                        }
                    ),
                    failure,
                ]
            ),
        )

        client.load_csrf("csrf_token")
        with self.assertRaises(HarnessFailure) as raised:
            client.send_json(
                "admit_player_1",
                "POST",
                "/api/release0/admission/enter",
                account_id=77,
            )

        self.assertNotIn(csrf_token, raised.exception.message)
        self.assertIn("<redacted-csrf-token>", raised.exception.message)

    def test_redacts_loaded_csrf_token_from_url_error_message(self):
        csrf_token = "csrf-token-123"
        client = HttpClient(
            "http://127.0.0.1:8081",
            "secret",
            timeout_seconds=1.0,
            opener=FakeOpener(
                [
                    FakeResponse(
                        {
                            "headerName": "X-CSRF-TOKEN",
                            "parameterName": "_csrf",
                            "token": csrf_token,
                        }
                    ),
                    error.URLError(f"debug echoed token {csrf_token}"),
                ]
            ),
        )

        client.load_csrf("csrf_token")
        with self.assertRaises(HarnessFailure) as raised:
            client.send_json(
                "admit_player_1",
                "POST",
                "/api/release0/admission/enter",
                account_id=77,
            )

        self.assertNotIn(csrf_token, raised.exception.message)
        self.assertIn("<redacted-csrf-token>", raised.exception.message)

    def test_redacts_queue_token_from_url_error_message(self):
        token = "raw-queue-token"
        client = HttpClient(
            "http://127.0.0.1:8081",
            "secret",
            timeout_seconds=1.0,
            opener=FakeOpener(
                [error.URLError(f"failed /api/release0/admission/queue/{token}")]
            ),
        )

        with self.assertRaises(HarnessFailure) as raised:
            client.send_json(
                "promote_player_3",
                "GET",
                f"/api/release0/admission/queue/{token}",
                account_id=77,
            )

        self.assertNotIn(token, raised.exception.message)
        self.assertIn("/api/release0/admission/queue/<redacted>", raised.exception.message)


class RunnerFlowTests(unittest.TestCase):
    def test_keeps_authenticated_capacity_sockets_open_until_queue_promotion(self):
        close_events = []
        snapshots = {}

        class FakeAuthenticatedClient:
            def __init__(self, label):
                self.label = label
                self.session_id = {"player1": 101, "player2": 102, "player3": 103}[label]
                self.initial_room_list = []

            def to_report(self):
                return {
                    "sessionId": self.session_id,
                    "initialRoomList": self.initial_room_list,
                }

            def close(self):
                if self.label not in close_events:
                    close_events.append(self.label)

        class FakeTcpProbe:
            def __enter__(self):
                return self

            def __exit__(self, _exc_type, _exc, _traceback):
                return False

        class FakeHttpClient:
            def load_csrf(self, step):
                snapshots[step] = list(close_events)
                return {
                    "headerName": "X-CSRF-TOKEN",
                    "parameterName": "_csrf",
                    "token": "csrf-token-123",
                }

            def send_empty(self, step, _method, _path, internal=False, account_id=None):
                if step == "test_identity_ready":
                    return 204
                if step == "direct_release_player_1":
                    snapshots[step] = list(close_events)
                    return 204
                raise AssertionError(f"unexpected empty step {step}")

            def send_json(
                self,
                step,
                _method,
                _path,
                body=None,
                internal=False,
                account_id=None,
            ):
                if step == "meta_health":
                    return {"status": "UP"}
                if step == "create_test_identities":
                    return {
                        "identities": [
                            {"accountId": 1, "nickname": "hf1"},
                            {"accountId": 2, "nickname": "hf2"},
                            {"accountId": 3, "nickname": "hf3"},
                        ]
                    }
                if step == "admit_player_1":
                    return self._admitted("token-player1")
                if step == "admit_player_2":
                    return self._admitted("token-player2")
                if step == "queue_player_3":
                    snapshots[step] = list(close_events)
                    return {"status": "Queued", "position": 1, "queueToken": "raw-queue-token"}
                if step == "promote_player_3":
                    snapshots[step] = list(close_events)
                    return self._admitted("token-player3")
                raise AssertionError(f"unexpected json step {step}")

            def _admitted(self, token):
                return {
                    "status": "Admitted",
                    "gameSessionToken": token,
                    "gameServerEndpoint": {
                        "host": "127.0.0.1",
                        "tcpPort": 40000,
                        "rudpPort": 40000,
                    },
                }

        def fake_authenticate(_host, _tcp_port, token, _timeout_seconds):
            return FakeAuthenticatedClient(token.removeprefix("token-"))

        def fake_connect(_address, timeout=None):
            return FakeTcpProbe()

        with tempfile.NamedTemporaryFile(delete=True) as report_file:
            args = SimpleNamespace(
                capacity=2,
                players=3,
                meta_base_url="http://127.0.0.1:8081",
                internal_token="secret",
                timeout_seconds=1.0,
                test_account_header="X-Release0-Test-Account",
                game_host="127.0.0.1",
                game_tcp_port=40000,
                nickname_prefix="hf",
                report=report_file.name,
            )

            exit_code = run(
                args,
                http_client=FakeHttpClient(),
                authenticate=fake_authenticate,
                connect=fake_connect,
            )
            report_payload = report_file.read().decode("utf-8")

        self.assertEqual(exit_code, 0)
        self.assertEqual(snapshots["csrf_token"], [])
        self.assertEqual(snapshots["queue_player_3"], [])
        self.assertEqual(snapshots["direct_release_player_1"], [])
        self.assertIn("player1", snapshots["promote_player_3"])
        self.assertNotIn("player2", snapshots["promote_player_3"])
        self.assertCountEqual(close_events, ["player1", "player2", "player3"])
        self.assertNotIn("raw-queue-token", report_payload)
        self.assertNotIn("csrf-token-123", report_payload)


if __name__ == "__main__":
    unittest.main()
