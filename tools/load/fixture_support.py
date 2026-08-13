"""Loopback Meta and Game process support for the public load fixture."""

from __future__ import annotations

import json
import os
import re
import selectors
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


READY = re.compile(r"^READY tcp=([0-9]+) udp=([0-9]+)$")


class MetaFixture:
    def __init__(self) -> None:
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()
        self._attempts: dict[str, str] = {}
        self._sessions: dict[str, int] = {}
        self._attempt_count = 0
        self._credential_count = 0

    @property
    def base_url(self) -> str:
        if self._server is None:
            raise RuntimeError("fixture is not running")
        return f"http://127.0.0.1:{self._server.server_port}"

    def __enter__(self) -> MetaFixture:
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                fixture._handle(self)

            def do_GET(self) -> None:
                fixture._handle(self)

            def log_message(self, *_args: object) -> None:
                return

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(
            target=self._server.serve_forever,
            name="portfolio-meta-fixture",
            daemon=True,
        )
        self._thread.start()
        return self

    def __exit__(self, *_args: object) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=2)
        self._server = None
        self._thread = None

    def _handle(self, request: BaseHTTPRequestHandler) -> None:
        try:
            if request.command == "POST" and request.path == "/api/v1/desktop-auth/attempts":
                self._start_auth(request)
            elif request.command == "POST" and request.path == "/api/v1/desktop-auth/exchanges":
                self._exchange_auth(request)
            elif request.command == "POST" and request.path == "/api/v1/game-credentials":
                self._issue_credential(request)
            elif request.command == "GET" and request.path == "/api/v1/collection":
                self._collection(request)
            else:
                self._json(request, 404, {"code": "NOT_FOUND"})
        except (KeyError, ValueError, json.JSONDecodeError):
            self._json(request, 400, {"code": "INVALID_REQUEST"})

    def _start_auth(self, request: BaseHTTPRequestHandler) -> None:
        body = self._body(request)
        callback = body["loopbackRedirectUri"]
        state = body["state"]
        challenge = body["codeChallenge"]
        parsed = urllib.parse.urlparse(callback)
        if (
            parsed.scheme != "http"
            or parsed.hostname != "127.0.0.1"
            or parsed.path != "/callback"
            or len(state) < 43
            or len(challenge) != 43
        ):
            raise ValueError("invalid desktop auth start")
        with self._lock:
            self._attempt_count += 1
            handoff = chr(ord("H") + self._attempt_count - 1) * 43
            self._attempts[handoff] = state
        query = urllib.parse.urlencode(
            {"callback": callback, "state": state, "handoff": handoff}
        )
        self._json(
            request,
            201,
            {
                "authorizationUrl": "https://fixture.invalid/authorize?" + query,
                "expiresAt": "2099-01-01T00:00:00Z",
            },
        )

    def _exchange_auth(self, request: BaseHTTPRequestHandler) -> None:
        body = self._body(request)
        with self._lock:
            expected = self._attempts.pop(body["handoffCode"], None)
            if expected != body["state"] or len(body["codeVerifier"]) < 43:
                raise ValueError("invalid desktop auth exchange")
            session = chr(ord("M") + len(self._sessions)) * 43
            self._sessions[session] = 0
        self._json(
            request,
            200,
            {"metaSession": session, "expiresAt": "2099-01-01T00:00:00Z"},
        )

    def _issue_credential(self, request: BaseHTTPRequestHandler) -> None:
        session = self._authorize(request)
        with self._lock:
            if session not in self._sessions or self._credential_count >= 10:
                raise ValueError("invalid credential issue")
            credential = chr(ord("A") + self._credential_count) * 43
            self._credential_count += 1
        self._json(
            request,
            201,
            {"credential": credential, "expiresAt": "2099-01-01T00:00:00Z"},
        )

    def _collection(self, request: BaseHTTPRequestHandler) -> None:
        session = self._authorize(request)
        with self._lock:
            count = self._sessions[session] + 1
            self._sessions[session] = count
        if count == 1:
            payload = self._collection_payload([], "0", 0)
        elif count == 2:
            payload = self._collection_payload([], "0", 1)
        else:
            payload = self._collection_payload(
                [{"itemId": "2", "quantity": "1", "value": "300"}], "300", 0
            )
        self._json(request, 200, payload)

    def _authorize(self, request: BaseHTTPRequestHandler) -> str:
        value = request.headers.get("Authorization", "")
        if not value.startswith("Bearer "):
            raise ValueError("missing authorization")
        session = value[len("Bearer ") :]
        with self._lock:
            if session not in self._sessions:
                raise ValueError("unknown session")
        return session

    @staticmethod
    def _collection_payload(items: list[object], wallet: str, pending: int) -> dict[str, object]:
        return {
            "items": items,
            "wallet": wallet,
            "pendingSettlementCount": pending,
            "freshness": "Fresh",
        }

    @staticmethod
    def _body(request: BaseHTTPRequestHandler) -> dict[str, str]:
        length = int(request.headers.get("Content-Length", "0"))
        if length <= 0 or length > 8192:
            raise ValueError("invalid body length")
        value = json.loads(request.rfile.read(length).decode("utf-8"))
        if not isinstance(value, dict) or any(
            not isinstance(key, str) or not isinstance(item, str)
            for key, item in value.items()
        ):
            raise ValueError("invalid body")
        return value

    @staticmethod
    def _json(
        request: BaseHTTPRequestHandler, status: int, payload: dict[str, object]
    ) -> None:
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        request.send_response(status)
        request.send_header("Content-Type", "application/json")
        request.send_header("Content-Length", str(len(encoded)))
        request.end_headers()
        request.wfile.write(encoded)


def _write_private(path: Path, text: str) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
        output.write(text)


def start_server(server: Path, directory: Path) -> tuple[subprocess.Popen[str], int, int]:
    credential = directory / "meta.credential"
    config = directory / "server.conf"
    _write_private(credential, "fixture-service-credential")
    _write_private(
        config,
        "\n".join(
            (
                "bind_address=127.0.0.1",
                "tcp_port=0",
                "udp_port=0",
                f"journal_path={directory / 'outbox.journal'}",
                "meta_claim_url=https://meta.test/internal/v1/game-credentials/claim",
                "meta_ca_certificate_file=/tmp/unused-meta-ca.pem",
                f"meta_ca_certificate_sha256={'a' * 64}",
                "meta_expected_hostname=meta.test",
                "meta_settlements_url=https://meta.test/internal/v1/settlements",
                f"meta_service_credential_file={credential}",
                "metrics_enabled=false",
                "worker_threads=2",
                "worker_queue_capacity=256",
                "deadline_capacity=128",
                "max_connections=16",
                "test_meta_fixture=true",
                "",
            )
        ),
    )
    process = subprocess.Popen(
        [str(server), "--config", str(config)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    if process.stdout is None:
        raise RuntimeError("game server stdout is unavailable")
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 10
    try:
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("game server exited before READY")
            for _key, _mask in selector.select(timeout=0.2):
                match = READY.fullmatch(process.stdout.readline().strip())
                if match:
                    tcp, udp = int(match.group(1)), int(match.group(2))
                    if not (0 < tcp <= 65535 and 0 < udp <= 65535):
                        raise RuntimeError("game server returned invalid ports")
                    return process, tcp, udp
        raise RuntimeError("game server READY timed out")
    except Exception:
        stop_process(process)
        raise
    finally:
        selector.close()


def stop_process(process: subprocess.Popen[str] | None) -> int | None:
    if process is None or process.poll() is not None:
        return process.returncode if process is not None else None
    process.terminate()
    try:
        return process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        return None
