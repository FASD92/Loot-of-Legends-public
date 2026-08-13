"""Bounded async wrapper for the accepted public Meta HTTP routes."""

from __future__ import annotations

import asyncio
import hashlib
import json
import re
import ssl
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path
from typing import Any


_OPAQUE_43 = re.compile(r"^[A-Za-z0-9_-]{43}$")
_PKCE_TEXT = re.compile(r"^[A-Za-z0-9._~-]{43,128}$")
_LOOPBACK = re.compile(
    r"^http://127\.0\.0\.1:(4915[2-9]|491[6-9][0-9]|49[2-9][0-9]{2}|"
    r"5[0-9]{4}|6[0-4][0-9]{3}|65[0-4][0-9]{2}|655[0-2][0-9]|6553[0-5])/callback$"
)
_PUBLIC_READ_PATH = re.compile(r"^/api/v1/[a-z0-9/-]{1,160}$")
_DIGEST = re.compile(r"^[0-9a-f]{64}$")


class MetaProtocolError(RuntimeError):
    def __init__(self, message: str, *, status: int | None = None, code: str | None = None) -> None:
        super().__init__(message)
        self.status = status
        self.code = code


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_args: object, **_kwargs: object) -> None:
        return None


class MetaHttpClient:
    def __init__(
        self,
        base_url: str,
        *,
        allow_insecure_loopback: bool = False,
        timeout_seconds: float = 5.0,
        maximum_response_bytes: int = 64 * 1024,
        ca_certificate_file: str | Path | None = None,
        ca_certificate_sha256: str | None = None,
    ) -> None:
        parsed = urllib.parse.urlsplit(base_url)
        loopback_http = (
            allow_insecure_loopback
            and parsed.scheme == "http"
            and parsed.hostname in {"127.0.0.1", "localhost"}
        )
        if parsed.scheme != "https" and not loopback_http:
            raise MetaProtocolError("Meta base URL must use HTTPS")
        if not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise MetaProtocolError("Meta base URL contains forbidden authority/query data")
        if parsed.path not in {"", "/"}:
            raise MetaProtocolError("Meta base URL must not contain a path")
        if timeout_seconds <= 0 or maximum_response_bytes < 1024:
            raise ValueError("invalid Meta client bounds")
        self._base_url = base_url.rstrip("/")
        self._timeout = timeout_seconds
        self._maximum_response_bytes = maximum_response_bytes
        if (ca_certificate_file is None) != (ca_certificate_sha256 is None):
            raise ValueError("CA file and SHA-256 must be supplied together")
        self._ssl_context: ssl.SSLContext | None = None
        if ca_certificate_file is not None:
            path = Path(ca_certificate_file)
            try:
                contents = path.read_bytes()
            except OSError as exc:
                raise MetaProtocolError("CA file is unavailable") from exc
            if (
                not contents
                or len(contents) > 256 * 1024
                or _DIGEST.fullmatch(ca_certificate_sha256 or "") is None
                or hashlib.sha256(contents).hexdigest() != ca_certificate_sha256
            ):
                raise MetaProtocolError("CA file digest is not approved")
            try:
                self._ssl_context = ssl.create_default_context(cafile=str(path))
            except (OSError, ssl.SSLError) as exc:
                raise MetaProtocolError("CA file is invalid") from exc

    async def start_desktop_auth(
        self,
        *,
        state: str,
        code_challenge: str,
        loopback_redirect_uri: str,
    ) -> dict[str, str]:
        _pkce(state, "state")
        _opaque(code_challenge, "codeChallenge")
        if _LOOPBACK.fullmatch(loopback_redirect_uri) is None:
            raise MetaProtocolError("loopbackRedirectUri violates the accepted contract")
        response = await self._request(
            "POST",
            "/api/v1/desktop-auth/attempts",
            body={
                "state": state,
                "codeChallenge": code_challenge,
                "loopbackRedirectUri": loopback_redirect_uri,
            },
            expected_status=201,
        )
        _exact_keys(response, {"authorizationUrl", "expiresAt"})
        authorization_url = response.get("authorizationUrl")
        if not isinstance(authorization_url, str) or urllib.parse.urlsplit(authorization_url).scheme != "https":
            raise MetaProtocolError("authorizationUrl must use HTTPS")
        _timestamp(response.get("expiresAt"), "expiresAt")
        return response

    async def exchange_desktop_auth(
        self,
        *,
        handoff_code: str,
        state: str,
        code_verifier: str,
    ) -> dict[str, str]:
        _opaque(handoff_code, "handoffCode")
        _pkce(state, "state")
        _pkce(code_verifier, "codeVerifier")
        response = await self._request(
            "POST",
            "/api/v1/desktop-auth/exchanges",
            body={
                "handoffCode": handoff_code,
                "state": state,
                "codeVerifier": code_verifier,
            },
            expected_status=200,
        )
        _exact_keys(response, {"metaSession", "expiresAt"})
        _opaque(response.get("metaSession"), "metaSession")
        _timestamp(response.get("expiresAt"), "expiresAt")
        return response

    async def issue_game_credential(self, meta_session: str) -> dict[str, str]:
        _opaque(meta_session, "metaSession")
        response = await self._request(
            "POST",
            "/api/v1/game-credentials",
            token=meta_session,
            expected_status=201,
        )
        _exact_keys(response, {"credential", "expiresAt"})
        _opaque(response.get("credential"), "credential")
        _timestamp(response.get("expiresAt"), "expiresAt")
        return response

    async def get_json(self, path: str, *, meta_session: str) -> dict[str, Any]:
        """Call an accepted authenticated public read route (for Collection)."""

        _opaque(meta_session, "metaSession")
        if _PUBLIC_READ_PATH.fullmatch(path) is None:
            raise MetaProtocolError("public read path is outside the bounded API namespace")
        return await self._request("GET", path, token=meta_session, expected_status=200)

    async def _request(
        self,
        method: str,
        path: str,
        *,
        body: dict[str, Any] | None = None,
        token: str | None = None,
        expected_status: int,
        timeout_seconds: float | None = None,
    ) -> dict[str, Any]:
        return await asyncio.to_thread(
            self._request_sync,
            method,
            path,
            body,
            token,
            expected_status,
            timeout_seconds,
        )

    def _request_sync(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None,
        token: str | None,
        expected_status: int,
        timeout_seconds: float | None,
    ) -> dict[str, Any]:
        encoded = None
        headers = {"Accept": "application/json"}
        if body is not None:
            encoded = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if token is not None:
            headers["Authorization"] = "Bearer " + token
        request = urllib.request.Request(
            self._base_url + path,
            data=encoded,
            headers=headers,
            method=method,
        )
        try:
            with urllib.request.build_opener(
                _NoRedirect, urllib.request.HTTPSHandler(context=self._ssl_context)
            ).open(
                request, timeout=self._timeout if timeout_seconds is None else timeout_seconds
            ) as response:
                status = response.status
                payload = response.read(self._maximum_response_bytes + 1)
                content_type = response.headers.get_content_type()
        except urllib.error.HTTPError as exc:
            try:
                payload = exc.read(self._maximum_response_bytes + 1)
            finally:
                exc.close()
            code = _error_code(payload)
            raise MetaProtocolError(
                f"Meta request rejected with HTTP {exc.code}", status=exc.code, code=code
            ) from exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise MetaProtocolError("Meta request failed at the network boundary") from exc
        if status != expected_status:
            raise MetaProtocolError(f"Meta response status {status} is not {expected_status}", status=status)
        if len(payload) > self._maximum_response_bytes:
            raise MetaProtocolError("Meta response exceeds maximum_response_bytes")
        if content_type != "application/json":
            raise MetaProtocolError("Meta response Content-Type is not application/json")
        try:
            value = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise MetaProtocolError("Meta response is not valid JSON") from exc
        if not isinstance(value, dict):
            raise MetaProtocolError("Meta response must be a JSON object")
        return value


def _exact_keys(value: dict[str, Any], expected: set[str]) -> None:
    if value.keys() != expected:
        raise MetaProtocolError(f"Meta response keys must be {sorted(expected)!r}")


def _opaque(value: object, field: str) -> None:
    if not isinstance(value, str) or _OPAQUE_43.fullmatch(value) is None:
        raise MetaProtocolError(f"{field} violates the opaque 43-byte contract")


def _pkce(value: object, field: str) -> None:
    if not isinstance(value, str) or _PKCE_TEXT.fullmatch(value) is None:
        raise MetaProtocolError(f"{field} violates the PKCE text contract")


def _timestamp(value: object, field: str) -> None:
    if not isinstance(value, str):
        raise MetaProtocolError(f"{field} must be a date-time")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise MetaProtocolError(f"{field} must be a date-time") from exc
    if parsed.tzinfo is None:
        raise MetaProtocolError(f"{field} must include a timezone")


def _error_code(payload: bytes) -> str | None:
    try:
        value = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    code = value.get("code") if isinstance(value, dict) else None
    return code if isinstance(code, str) and len(code) <= 64 else None
