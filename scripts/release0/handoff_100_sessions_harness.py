#!/usr/bin/env python3
"""Release 0 100-session handoff evidence harness."""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.release0.handoff_foundation_harness import (
    HarnessFailure,
    HttpClient,
    authenticate_cxx,
    monotonic_ms,
    redact_admission,
    require_admitted,
    require_status,
    utc_now,
    write_report,
)


_RAW_TOKEN_KEYS = {
    "gameSessionToken",
    "queueToken",
    "csrfToken",
    "csrf_token",
    "token",
}
_QUEUE_TOKEN_PATH = re.compile(r"(/api/release0/admission/queue/)[^/?#\s>\"']+")
_TOKEN_ASSIGNMENT = re.compile(r"\b(gameSessionToken|queueToken)=([^,\s\"'}]+)")


def _sanitize_report_value(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: _sanitize_report_value(inner_value)
            for key, inner_value in value.items()
            if key not in _RAW_TOKEN_KEYS
        }
    if isinstance(value, list):
        return [_sanitize_report_value(item) for item in value]
    if isinstance(value, str):
        value = _QUEUE_TOKEN_PATH.sub(r"\1<redacted>", value)
        return _TOKEN_ASSIGNMENT.sub(r"\1=<redacted>", value)
    return value


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
            "runName": "handoff-100-sessions",
            "status": self.status,
            "startedAt": self.started_at,
            "finishedAt": self.finished_at,
            "failedStep": self.failed_step,
            "capacity": self.capacity,
            "players": self.players,
            "steps": _sanitize_report_value(cleaned_steps),
            "playerResults": _sanitize_report_value(self.player_results),
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


def redact_queued_admission(
    queued: dict[str, Any],
    cxx_connect_attempted: bool,
) -> dict[str, Any]:
    return {
        "status": queued.get("status"),
        "position": queued.get("position"),
        "queueTokenReceived": bool(queued.get("queueToken")),
        "gameSessionTokenReceived": bool(queued.get("gameSessionToken")),
        "cxxConnectAttemptedWhileQueued": cxx_connect_attempted,
    }


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


def _record_unexpected_failure(report: Report, exc: Exception) -> None:
    step = "unexpected_error"
    if report.steps and report.steps[-1]["status"] == "RUNNING":
        step = report.steps[-1]["name"]
    failure = HarnessFailure(step, f"unexpected {type(exc).__name__}")
    _record_failure(report, failure)


def _identity_summary(identities: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        {"accountId": item["accountId"], "nickname": item["nickname"]}
        for item in identities
    ]


def _require_queue_head(step: str, queued: dict[str, Any]) -> None:
    require_status(step, queued, "Queued")
    if queued.get("position") != 1 or not queued.get("queueToken"):
        raise HarnessFailure(
            step,
            "queued player did not receive position 1 and queueToken",
        )
    if queued.get("gameSessionToken"):
        raise HarnessFailure(step, "queued player unexpectedly received gameSessionToken")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Release 0 100-session handoff evidence.")
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
    parser.add_argument("--capacity", type=int, default=100)
    parser.add_argument("--players", type=int, default=101)
    parser.add_argument("--nickname-prefix", default="h100")
    parser.add_argument("--timeout-seconds", type=float, default=5.0)
    parser.add_argument("--report", default="artifacts/release0/handoff_100_sessions_report.json")
    args = parser.parse_args(argv)
    if not args.internal_token:
        parser.error("--internal-token or META_INTERNAL_TOKEN is required")
    if args.capacity != 100 or args.players != 101:
        parser.error("100-session run requires --capacity 100 --players 101")
    return args


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
    connect_tcp = connect
    if connect_tcp is None:
        import socket

        connect_tcp = socket.create_connection

    active_cxx_clients: list[Any] = []
    admitted_clients: list[Any] = []
    admitted_players: list[dict[str, Any]] = []
    exit_code = 1

    try:
        if args.capacity != 100 or args.players != 101:
            raise HarnessFailure(
                "proof_shape",
                "100-session run requires capacity=100 and players=101",
            )

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
                "identityCount": len(identities),
                "identities": _identity_summary(identities),
            },
        )

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
            admitted_clients.append(cxx)
            cxx_report = cxx.to_report()
            player_result = {
                "label": f"player{index + 1}",
                "accountId": identity["accountId"],
                "nickname": identity["nickname"],
                "admission": redact_admission(admission),
                "cxx": cxx_report,
            }
            admitted_players.append(player_result)
            report.player_results.append(player_result)
            report.pass_step(step, {"sessionId": cxx_report["sessionId"]})

        report.start_step("authenticated_100_residency")
        report.pass_step(
            "authenticated_100_residency",
            {"authenticatedTcpClients": len(admitted_clients)},
        )

        queued_identity = identities[args.capacity]
        report.start_step("queue_player_101", {"accountId": queued_identity["accountId"]})
        queued = http.send_json(
            "queue_player_101",
            "POST",
            "/api/release0/admission/enter",
            account_id=queued_identity["accountId"],
        )
        _require_queue_head("queue_player_101", queued)
        queued_admission = redact_queued_admission(queued, cxx_connect_attempted=False)
        report.player_results.append(
            {
                "label": "player101",
                "accountId": queued_identity["accountId"],
                "nickname": queued_identity["nickname"],
                "queuedAdmission": queued_admission,
            }
        )
        report.pass_step(
            "queue_player_101",
            {
                "position": queued["position"],
                "cxxConnectAttemptedWhileQueued": False,
            },
        )

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

        report.start_step("promote_player_101", {"accountId": queued_identity["accountId"]})
        promoted = http.send_json(
            "promote_player_101",
            "GET",
            f"/api/release0/admission/queue/{queued['queueToken']}",
            account_id=queued_identity["accountId"],
        )
        require_admitted("promote_player_101", promoted)
        cxx = authenticate_client(
            promoted["gameServerEndpoint"]["host"],
            int(promoted["gameServerEndpoint"]["tcpPort"]),
            promoted["gameSessionToken"],
            args.timeout_seconds,
        )
        active_cxx_clients.append(cxx)
        cxx_report = cxx.to_report()
        report.player_results[-1]["promotedAdmission"] = redact_admission(promoted)
        report.player_results[-1]["cxx"] = cxx_report
        report.pass_step("promote_player_101", {"sessionId": cxx_report["sessionId"]})

        report.pass_run()
        exit_code = 0
    except HarnessFailure as failure:
        _record_failure(report, failure)
    except Exception as exc:
        _record_unexpected_failure(report, exc)
    finally:
        for client in reversed(active_cxx_clients):
            _close_cxx_client(client)
        write_report(Path(args.report), report)
    return exit_code


def main(argv: list[str]) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
