#!/usr/bin/env python3
"""Release 0 handoff foundation evidence harness."""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import ssl
import struct
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from http.cookiejar import CookieJar
from pathlib import Path
from typing import Any
from urllib import error, request


_QUEUE_TOKEN_PATH = re.compile(r"(/api/release0/admission/queue/)[^/?#\s>]+")


class HarnessFailure(RuntimeError):
    """Failure with a step name that can be recorded in the report."""

    def __init__(self, step: str, message: str):
        super().__init__(message)
        self.step = step
        self.message = message


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def monotonic_ms() -> int:
    return int(time.monotonic() * 1000)


@dataclass
class Report:
    capacity: int
    players: int
    status: str = "RUNNING"
    started_at: str = field(default_factory=utc_now)
    finished_at: str | None = None
    failed_step: str | None = None
    steps: list[dict[str, Any]] = field(default_factory=list)
    player_results: list[dict[str, Any]] = field(default_factory=list)

    def start_step(self, name: str, details: dict[str, Any] | None = None) -> None:
        self.steps.append(
            {
                "name": name,
                "status": "RUNNING",
                "startedAt": utc_now(),
                "finishedAt": None,
                "latencyMs": None,
                "error": None,
                "details": details or {},
                "_startedMs": monotonic_ms(),
            }
        )

    def pass_step(self, name: str, details: dict[str, Any] | None = None) -> None:
        step = self._current_step(name)
        self._finish_step(step, "PASS", None, details)

    def fail_step(self, name: str, error_message: str) -> None:
        step = self._current_step(name)
        self._finish_step(step, "FAIL", error_message, None)
        self.status = "FAIL"
        self.failed_step = name
        self.finished_at = utc_now()

    def pass_run(self) -> None:
        self.status = "PASS"
        self.finished_at = utc_now()

    def to_json(self) -> dict[str, Any]:
        cleaned_steps = []
        for step in self.steps:
            cleaned = dict(step)
            cleaned.pop("_startedMs", None)
            cleaned_steps.append(cleaned)
        return {
            "runName": "handoff-foundation",
            "status": self.status,
            "startedAt": self.started_at,
            "finishedAt": self.finished_at,
            "failedStep": self.failed_step,
            "capacity": self.capacity,
            "players": self.players,
            "steps": cleaned_steps,
            "playerResults": self.player_results,
        }

    def _current_step(self, name: str) -> dict[str, Any]:
        if not self.steps or self.steps[-1]["name"] != name:
            raise HarnessFailure(name, f"step {name} was not started")
        return self.steps[-1]

    def _finish_step(
        self,
        step: dict[str, Any],
        status: str,
        error_message: str | None,
        details: dict[str, Any] | None,
    ) -> None:
        step["status"] = status
        step["finishedAt"] = utc_now()
        step["latencyMs"] = monotonic_ms() - int(step.pop("_startedMs"))
        step["error"] = error_message
        if details:
            step["details"].update(details)


class TcpProtocol:
    HEADER_SIZE = 4
    AUTHENTICATE_GAME_SESSION = 0x0003
    WELCOME = 0x0001
    ROOM_LIST_SNAPSHOT = 0x0107
    ROOM_ENTRY_SIZE = 9

    @staticmethod
    def serialize_authenticate_game_session(game_session_token: str) -> bytes:
        token = game_session_token.encode("utf-8")
        if not token or len(token) > 512:
            raise HarnessFailure("cxx_auth", "game session token length is invalid")
        packet_size = TcpProtocol.HEADER_SIZE + 2 + len(token)
        return struct.pack(">HHH", packet_size, TcpProtocol.AUTHENTICATE_GAME_SESSION, len(token)) + token

    @staticmethod
    def parse_welcome(packet: bytes) -> int:
        if len(packet) != 12:
            raise HarnessFailure("cxx_welcome", "invalid Welcome packet")
        size, packet_type, session_id = struct.unpack(">HHQ", packet)
        if size != 12 or packet_type != TcpProtocol.WELCOME or session_id == 0:
            raise HarnessFailure("cxx_welcome", "invalid Welcome packet")
        return session_id

    @staticmethod
    def parse_room_list_snapshot(packet: bytes) -> list[dict[str, Any]]:
        if len(packet) < 6:
            raise HarnessFailure("cxx_room_list_snapshot", "invalid RoomListSnapshot packet")
        size, packet_type, count = struct.unpack(">HHH", packet[:6])
        expected_size = 6 + (count * TcpProtocol.ROOM_ENTRY_SIZE)
        if size != len(packet) or packet_type != TcpProtocol.ROOM_LIST_SNAPSHOT or expected_size != len(packet):
            raise HarnessFailure("cxx_room_list_snapshot", "invalid RoomListSnapshot packet")
        rooms = []
        offset = 6
        for _ in range(count):
            room_id, player_count, max_players, raw_status = struct.unpack(">IHHB", packet[offset:offset + 9])
            if raw_status == 0:
                status = "OPEN"
            elif raw_status == 1:
                status = "IN_PROGRESS"
            else:
                raise HarnessFailure("cxx_room_list_snapshot", "unknown room status")
            rooms.append(
                {
                    "roomId": room_id,
                    "playerCount": player_count,
                    "maxPlayers": max_players,
                    "roomStatus": status,
                }
            )
            offset += 9
        return rooms


def redact_admission(admission: dict[str, Any]) -> dict[str, Any]:
    redacted = dict(admission)
    redacted["gameSessionTokenReceived"] = bool(redacted.pop("gameSessionToken", None))
    return redacted


def _redact_path(text: str) -> str:
    return _QUEUE_TOKEN_PATH.sub(r"\1<redacted>", text)


@dataclass(eq=False)
class AuthenticatedCxxClient:
    sock: socket.socket
    session_id: int
    initial_room_list: list[dict[str, Any]]
    closed: bool = False

    def to_report(self) -> dict[str, Any]:
        return {
            "sessionId": self.session_id,
            "initialRoomList": self.initial_room_list,
        }

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        self.sock.close()


class HttpClient:
    def __init__(
        self,
        base_url: str,
        internal_token: str,
        timeout_seconds: float,
        test_account_header: str = "X-Release0-Test-Account",
        opener: Any | None = None,
    ):
        self.base_url = base_url.rstrip("/")
        self.internal_token = internal_token
        self.timeout_seconds = timeout_seconds
        self.test_account_header = test_account_header
        self.csrf_header_name: str | None = None
        self.csrf_token: str | None = None
        self.cookie_jar = CookieJar()
        self.opener = opener or request.build_opener(request.HTTPCookieProcessor(self.cookie_jar))

    def build_request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None,
        internal: bool,
        account_id: int | None,
    ) -> request.Request:
        data = None
        headers = {}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if internal:
            headers["X-Internal-Token"] = self.internal_token
        if (
            not internal
            and method.upper() in {"POST", "PUT", "PATCH", "DELETE"}
            and self.csrf_header_name
            and self.csrf_token
        ):
            headers[self.csrf_header_name] = self.csrf_token
        if account_id is not None:
            headers[self.test_account_header] = str(account_id)
        return request.Request(
            self.base_url + path,
            data=data,
            headers=headers,
            method=method,
        )

    def load_csrf(self, step: str) -> dict[str, Any]:
        csrf = self.send_json(step, "GET", "/api/release0/auth/csrf")
        header_name = csrf.get("headerName")
        token = csrf.get("token")
        if not header_name or not token:
            raise HarnessFailure(step, "CSRF response is incomplete")
        self.csrf_header_name = header_name
        self.csrf_token = token
        return csrf

    def redact_text(self, text: str) -> str:
        redacted = _redact_path(text)
        if self.csrf_token:
            redacted = redacted.replace(self.csrf_token, "<redacted-csrf-token>")
        return redacted

    def send_json(
        self,
        step: str,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
        internal: bool = False,
        account_id: int | None = None,
    ) -> dict[str, Any]:
        req = self.build_request(method, path, body, internal, account_id)
        try:
            with self.opener.open(req, timeout=self.timeout_seconds) as response:
                payload = response.read()
                if not payload:
                    return {}
                return json.loads(payload.decode("utf-8"))
        except error.HTTPError as exc:
            message = exc.read().decode("utf-8", errors="replace")
            raise HarnessFailure(
                step,
                f"HTTP {exc.code} for {self.redact_text(path)}: {self.redact_text(message)}",
            ) from exc
        except (error.URLError, TimeoutError, json.JSONDecodeError) as exc:
            raise HarnessFailure(
                step,
                f"request failed for {self.redact_text(path)}: {self.redact_text(str(exc))}",
            ) from exc

    def send_empty(
        self,
        step: str,
        method: str,
        path: str,
        internal: bool = False,
        account_id: int | None = None,
    ) -> int:
        req = self.build_request(method, path, None, internal, account_id)
        try:
            with self.opener.open(req, timeout=self.timeout_seconds) as response:
                response.read()
                return response.status
        except error.HTTPError as exc:
            raise HarnessFailure(step, f"HTTP {exc.code} for {self.redact_text(path)}") from exc
        except (error.URLError, TimeoutError) as exc:
            raise HarnessFailure(
                step,
                f"request failed for {self.redact_text(path)}: {self.redact_text(str(exc))}",
            ) from exc


def recv_exact(sock: socket.socket, size: int, step: str) -> bytes:
    chunks = []
    remaining = size
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise HarnessFailure(step, "TCP connection closed before expected packet")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_tcp_packet(sock: socket.socket, step: str) -> bytes:
    header = recv_exact(sock, TcpProtocol.HEADER_SIZE, step)
    size, _packet_type = struct.unpack(">HH", header)
    if size < TcpProtocol.HEADER_SIZE or size > 4096:
        raise HarnessFailure(step, f"invalid TCP packet size {size}")
    return header + recv_exact(sock, size - TcpProtocol.HEADER_SIZE, step)


def authenticate_cxx(
    host: str,
    tcp_port: int,
    token: str,
    timeout_seconds: float,
) -> AuthenticatedCxxClient:
    sock = None
    try:
        sock = socket.create_connection((host, tcp_port), timeout=timeout_seconds)
        sock.settimeout(timeout_seconds)
        sock.sendall(TcpProtocol.serialize_authenticate_game_session(token))
        welcome_packet = recv_tcp_packet(sock, "cxx_welcome")
        session_id = TcpProtocol.parse_welcome(welcome_packet)
        room_list_packet = recv_tcp_packet(sock, "cxx_room_list_snapshot")
        rooms = TcpProtocol.parse_room_list_snapshot(room_list_packet)
        return AuthenticatedCxxClient(sock=sock, session_id=session_id, initial_room_list=rooms)
    except HarnessFailure:
        if sock is not None:
            _close_cxx_client(sock)
        raise
    except (OSError, TimeoutError) as exc:
        if sock is not None:
            _close_cxx_client(sock)
        raise HarnessFailure("cxx_auth", f"TCP authentication failed: {exc}") from exc


def require_status(step: str, response: dict[str, Any], expected: str) -> None:
    actual = response.get("status")
    if actual != expected:
        raise HarnessFailure(step, f"expected {expected}, got {actual}")


def require_admitted(step: str, response: dict[str, Any]) -> dict[str, Any]:
    require_status(step, response, "Admitted")
    if not response.get("gameSessionToken"):
        raise HarnessFailure(step, "Admitted response has no gameSessionToken")
    endpoint = response.get("gameServerEndpoint")
    if not isinstance(endpoint, dict):
        raise HarnessFailure(step, "Admitted response has no gameServerEndpoint")
    if not endpoint.get("host") or not endpoint.get("tcpPort"):
        raise HarnessFailure(step, "gameServerEndpoint is incomplete")
    return response


def write_report(path: Path, report: Report) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report.to_json(), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def _close_cxx_client(client: Any) -> None:
    try:
        client.close()
    except OSError:
        pass


def _remove_cxx_client(clients: list[Any], target: Any) -> None:
    clients[:] = [client for client in clients if client is not target]


def _record_failure(report: Report, failure: HarnessFailure) -> None:
    if report.status == "FAIL":
        return
    if report.steps and report.steps[-1]["status"] == "RUNNING":
        current_name = report.steps[-1]["name"]
        message = failure.message
        if failure.step != current_name:
            message = f"{failure.step}: {failure.message}"
        report.fail_step(current_name, message)
        return
    report.start_step(failure.step)
    report.fail_step(failure.step, failure.message)


def run(
    args: argparse.Namespace,
    http_client: HttpClient | None = None,
    authenticate: Any | None = None,
    connect: Any | None = None,
) -> int:
    report = Report(capacity=args.capacity, players=args.players)
    http = http_client or HttpClient(
        args.meta_base_url,
        args.internal_token,
        args.timeout_seconds,
        args.test_account_header,
    )
    authenticate_client = authenticate or authenticate_cxx
    connect_tcp = connect or socket.create_connection
    active_cxx_clients: list[Any] = []
    try:
        if args.capacity != 2 or args.players != 3:
            raise HarnessFailure("foundation_shape", "foundation run requires capacity=2 and players=3")

        report.start_step("meta_health")
        health = http.send_json("meta_health", "GET", "/actuator/health")
        if health.get("status") != "UP":
            raise HarnessFailure("meta_health", f"Meta health is {health.get('status')}")
        report.pass_step("meta_health", {"status": health.get("status")})

        report.start_step("csrf_token")
        csrf = http.load_csrf("csrf_token")
        report.pass_step(
            "csrf_token",
            {
                "headerName": csrf.get("headerName"),
                "parameterName": csrf.get("parameterName"),
            },
        )

        report.start_step("test_identity_ready")
        http.send_empty(
            "test_identity_ready",
            "GET",
            "/internal/release0/test-admission-identities/ready",
            internal=True,
        )
        report.pass_step("test_identity_ready")

        report.start_step("cxx_tcp_connect")
        try:
            with connect_tcp(
                (args.game_host, args.game_tcp_port),
                timeout=args.timeout_seconds,
            ):
                pass
        except (OSError, TimeoutError) as exc:
            raise HarnessFailure("cxx_tcp_connect", f"TCP connect failed: {exc}") from exc
        report.pass_step(
            "cxx_tcp_connect",
            {"host": args.game_host, "tcpPort": args.game_tcp_port},
        )

        report.start_step("create_test_identities")
        identity_response = http.send_json(
            "create_test_identities",
            "POST",
            "/internal/release0/test-admission-identities",
            {"count": args.players, "nicknamePrefix": args.nickname_prefix},
            internal=True,
        )
        identities = identity_response.get("identities", [])
        if len(identities) != args.players:
            raise HarnessFailure("create_test_identities", "identity count mismatch")
        report.pass_step(
            "create_test_identities",
            {
                "identities": [
                    {"accountId": item["accountId"], "nickname": item["nickname"]}
                    for item in identities
                ]
            },
        )

        admitted_players = []
        admitted_clients = []
        for index in range(args.capacity):
            identity = identities[index]
            step = f"admit_player_{index + 1}"
            report.start_step(
                step,
                {"accountId": identity["accountId"], "nickname": identity["nickname"]},
            )
            admission = http.send_json(
                step,
                "POST",
                "/api/release0/admission/enter",
                account_id=identity["accountId"],
            )
            require_admitted(step, admission)
            cxx = authenticate_client(
                admission["gameServerEndpoint"]["host"],
                int(admission["gameServerEndpoint"]["tcpPort"]),
                admission["gameSessionToken"],
                args.timeout_seconds,
            )
            active_cxx_clients.append(cxx)
            cxx_report = cxx.to_report()
            player_result = {
                "label": f"player{index + 1}",
                "accountId": identity["accountId"],
                "nickname": identity["nickname"],
                "admission": redact_admission(admission),
                "cxx": cxx_report,
            }
            admitted_players.append(player_result)
            admitted_clients.append(cxx)
            report.player_results.append(player_result)
            report.pass_step(step, {"sessionId": cxx_report["sessionId"]})

        queued_identity = identities[args.capacity]
        report.start_step("queue_player_3", {"accountId": queued_identity["accountId"]})
        queued = http.send_json(
            "queue_player_3",
            "POST",
            "/api/release0/admission/enter",
            account_id=queued_identity["accountId"],
        )
        require_status("queue_player_3", queued, "Queued")
        if queued.get("position") != 1 or not queued.get("queueToken"):
            raise HarnessFailure(
                "queue_player_3",
                "queued player did not receive position 1 and queueToken",
            )
        report.pass_step("queue_player_3", {"position": queued["position"]})

        release_account_id = admitted_players[0]["accountId"]
        report.start_step("direct_release_player_1", {"accountId": release_account_id})
        http.send_empty(
            "direct_release_player_1",
            "POST",
            f"/internal/release0/test-admission-identities/{release_account_id}/release-active-session",
            internal=True,
        )
        report.pass_step("direct_release_player_1")
        _close_cxx_client(admitted_clients[0])
        _remove_cxx_client(active_cxx_clients, admitted_clients[0])

        report.start_step("promote_player_3", {"accountId": queued_identity["accountId"]})
        promoted = http.send_json(
            "promote_player_3",
            "GET",
            f"/api/release0/admission/queue/{queued['queueToken']}",
            account_id=queued_identity["accountId"],
        )
        require_admitted("promote_player_3", promoted)
        cxx = authenticate_client(
            promoted["gameServerEndpoint"]["host"],
            int(promoted["gameServerEndpoint"]["tcpPort"]),
            promoted["gameSessionToken"],
            args.timeout_seconds,
        )
        active_cxx_clients.append(cxx)
        cxx_report = cxx.to_report()
        report.player_results.append(
            {
                "label": "player3",
                "accountId": queued_identity["accountId"],
                "nickname": queued_identity["nickname"],
                "admission": redact_admission(promoted),
                "cxx": cxx_report,
            }
        )
        report.pass_step("promote_player_3", {"sessionId": cxx_report["sessionId"]})

        report.pass_run()
        return 0
    except HarnessFailure as failure:
        _record_failure(report, failure)
        return 1
    finally:
        write_report(Path(args.report), report)
        for client in reversed(active_cxx_clients):
            _close_cxx_client(client)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Release 0 handoff foundation evidence.")
    parser.add_argument(
        "--meta-base-url",
        default=os.environ.get("META_BASE_URL", "http://127.0.0.1:8081"),
    )
    parser.add_argument("--internal-token", default=os.environ.get("META_INTERNAL_TOKEN", ""))
    parser.add_argument(
        "--test-account-header",
        default=os.environ.get("RELEASE0_TEST_ACCOUNT_HEADER", "X-Release0-Test-Account"),
    )
    parser.add_argument("--game-host", default=os.environ.get("RELEASE0_GAME_HOST", "127.0.0.1"))
    parser.add_argument(
        "--game-tcp-port",
        type=int,
        default=int(os.environ.get("RELEASE0_GAME_TCP_PORT", "40000")),
    )
    parser.add_argument("--capacity", type=int, default=2)
    parser.add_argument("--players", type=int, default=3)
    parser.add_argument("--nickname-prefix", default="hf")
    parser.add_argument("--timeout-seconds", type=float, default=5.0)
    parser.add_argument("--report", default="artifacts/release0/handoff_foundation_report.json")
    args = parser.parse_args(argv)
    if not args.internal_token:
        parser.error("--internal-token or META_INTERNAL_TOKEN is required")
    if args.capacity != 2 or args.players != 3:
        parser.error("foundation run requires --capacity 2 --players 3")
    return args


def main(argv: list[str]) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
