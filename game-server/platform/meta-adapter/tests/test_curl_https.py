from __future__ import annotations

import hashlib
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


PROBE = Path(sys.argv.pop(1)).resolve()
AUTHORIZATION = "Bearer fixture-secret-not-an-argument"


def _run(*arguments: object) -> None:
    subprocess.run(
        [str(argument) for argument in arguments],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class _Certificates:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.ca = root / "ca.pem"
        self.ca_key = root / "ca.key"
        self.server = root / "server.pem"
        self.server_key = root / "server.key"
        self.expired = root / "expired.pem"
        self.other_ca = root / "other-ca.pem"
        self._create()

    def _create(self) -> None:
        _run(
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            self.ca_key,
            "-out",
            self.ca,
            "-subj",
            "/CN=Loot Test CA",
            "-days",
            "2",
        )
        request = self.root / "server.csr"
        _run(
            "openssl",
            "req",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            self.server_key,
            "-out",
            request,
            "-subj",
            "/CN=localhost",
        )
        extensions = self.root / "server.ext"
        extensions.write_text(
            "basicConstraints=CA:FALSE\n"
            "keyUsage=digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\n"
            "subjectAltName=DNS:localhost\n"
        )
        _run(
            "openssl",
            "x509",
            "-req",
            "-in",
            request,
            "-CA",
            self.ca,
            "-CAkey",
            self.ca_key,
            "-CAcreateserial",
            "-out",
            self.server,
            "-days",
            "2",
            "-sha256",
            "-extfile",
            extensions,
        )
        self._create_expired(request)
        _run(
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            self.root / "other-ca.key",
            "-out",
            self.other_ca,
            "-subj",
            "/CN=Other Test CA",
            "-days",
            "2",
        )

    def _create_expired(self, request: Path) -> None:
        database = self.root / "index.txt"
        database.write_text("")
        (self.root / "serial").write_text("1000\n")
        (self.root / "newcerts").mkdir()
        config = self.root / "ca.cnf"
        config.write_text(
            f"""
[ca]
default_ca=CA_default
[CA_default]
database={database}
new_certs_dir={self.root / 'newcerts'}
certificate={self.ca}
private_key={self.ca_key}
serial={self.root / 'serial'}
default_md=sha256
policy=policy_any
unique_subject=no
[policy_any]
commonName=supplied
[server_ext]
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost
""".strip()
            + "\n"
        )
        _run(
            "openssl",
            "ca",
            "-batch",
            "-config",
            config,
            "-in",
            request,
            "-out",
            self.expired,
            "-startdate",
            "20200101000000Z",
            "-enddate",
            "20200102000000Z",
            "-extensions",
            "server_ext",
        )


class _HttpsFixture:
    def __init__(self, certificate: Path, key: Path) -> None:
        self.requests: list[tuple[str, str]] = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                owner.requests.append((self.path, self.headers.get("Authorization", "")))
                if self.path == "/slow":
                    time.sleep(2.2)
                if self.path == "/redirect":
                    self.send_response(302)
                    self.send_header("Location", "/ok")
                    self.end_headers()
                    return
                if self.path == "/oversized":
                    body = b"{" + b" " * (64 * 1024) + b"}"
                elif self.path == "/error":
                    body = b'{"code":"UNAVAILABLE"}'
                else:
                    body = b'{"ok":true}'
                self.send_response(503 if self.path == "/error" else 200)
                self.send_header(
                    "Content-Type",
                    "text/plain" if self.path == "/wrong-content" else "application/json",
                )
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ssl.SSLError):
                    pass

            def log_message(self, _format: str, *_args: object) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> _HttpsFixture:
        self.thread.start()
        return self

    def __exit__(self, *_args: object) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def url(self, path: str, hostname: str = "localhost") -> str:
        return f"https://{hostname}:{self.server.server_port}{path}"


class CurlHttpsExchangeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        cls.certificates = _Certificates(Path(cls.temporary.name))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def probe(
        self,
        url: str,
        *,
        ca: Path | None = None,
        digest: str | None = None,
        hostname: str = "localhost",
    ) -> str:
        ca = ca or self.certificates.ca
        completed = subprocess.run(
            [str(PROBE), url, str(ca), digest or _sha256(ca), hostname],
            check=True,
            capture_output=True,
            text=True,
            timeout=6,
        )
        self.assertNotIn("fixture-secret", completed.stdout + completed.stderr)
        return completed.stdout.strip()

    def test_verified_ca_success_non_2xx_and_redirect_are_typed_without_following(self) -> None:
        with _HttpsFixture(
            self.certificates.server, self.certificates.server_key
        ) as fixture:
            self.assertEqual(
                "status=Response code=200 bodyBytes=11", self.probe(fixture.url("/ok"))
            )
            self.assertEqual(AUTHORIZATION, fixture.requests[-1][1])
            self.assertEqual(
                "status=Response code=503 bodyBytes=22", self.probe(fixture.url("/error"))
            )
            before = len(fixture.requests)
            self.assertEqual(
                "status=Response code=302 bodyBytes=0", self.probe(fixture.url("/redirect"))
            )
            self.assertEqual(before + 1, len(fixture.requests))

    def test_policy_bounds_reject_timeout_oversize_content_type_and_config_mismatch(self) -> None:
        with _HttpsFixture(
            self.certificates.server, self.certificates.server_key
        ) as fixture:
            self.assertEqual(
                "status=Timeout code=0 bodyBytes=0", self.probe(fixture.url("/slow"))
            )
            for path in ("/oversized", "/wrong-content"):
                self.assertEqual(
                    "status=PolicyFailure code=0 bodyBytes=0",
                    self.probe(fixture.url(path)),
                )
            self.assertEqual(
                "status=PolicyFailure code=0 bodyBytes=0",
                self.probe(fixture.url("/ok"), digest="0" * 64),
            )
            self.assertEqual(
                "status=PolicyFailure code=0 bodyBytes=0",
                self.probe(fixture.url("/ok"), hostname="other.internal"),
            )

    def test_wrong_ca_hostname_and_expired_certificate_fail_closed(self) -> None:
        with _HttpsFixture(
            self.certificates.server, self.certificates.server_key
        ) as fixture:
            self.assertEqual(
                "status=NetworkFailure code=0 bodyBytes=0",
                self.probe(fixture.url("/ok"), ca=self.certificates.other_ca),
            )
            self.assertEqual(
                "status=NetworkFailure code=0 bodyBytes=0",
                self.probe(fixture.url("/ok", hostname="127.0.0.1"), hostname="127.0.0.1"),
            )
        with _HttpsFixture(
            self.certificates.expired, self.certificates.server_key
        ) as fixture:
            self.assertEqual(
                "status=NetworkFailure code=0 bodyBytes=0", self.probe(fixture.url("/ok"))
            )


if __name__ == "__main__":
    unittest.main()
