"""Sanitized, checksum-backed, no-clobber evidence packages."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import tempfile
from pathlib import Path
from typing import Any, Iterable

from .contracts import canonical_json_bytes, document_digest, validate_document


_IDENTITY_KEYS = {"accountid", "sessionid", "nickname", "email", "googleidentity"}
_SECRET_KEYS = {
    "authorization",
    "bindcapability",
    "capability",
    "cookie",
    "credential",
    "dbpassword",
    "password",
    "provideridentity",
    "token",
}
_NETWORK_KEYS = {"address", "endpoint", "host", "ip", "privateendpoint", "url"}
_PATH_KEYS = {"secretpath"}
_STACK_KEYS = {"exception", "stack", "stacktrace", "traceback"}
_LEAK_PATTERNS = (
    re.compile(rb"(?i)bearer\s+[A-Za-z0-9._~+/=-]+"),
    re.compile(rb"(?i)\b(?:authorization|cookie|credential|token|password)\s*[:=]\s*[^\s,;]+"),
    re.compile(rb"\b[A-Za-z0-9_-]{43}\b"),
    re.compile(rb"\b(?:\d{1,3}\.){3}\d{1,3}\b"),
    re.compile(rb"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}"),
    re.compile(rb"https?://[^\s\"'<>]+"),
    re.compile(rb"/(?:Users|home|root|opt)/[^\s\"'<>]+"),
    re.compile(rb"\b[0-9a-fA-F]{8}-[0-9a-fA-F-]{27,}\b"),
    re.compile(rb"(?m)^\s*at\s+[A-Za-z_$][A-Za-z0-9_.$]*(?:\([^\n]*\))?"),
)


class EvidenceSanitizer:
    def __init__(self) -> None:
        self._aliases: dict[str, str] = {}

    def sanitize_bytes(self, path: Path, raw: bytes) -> bytes:
        try:
            if path.suffix == ".json":
                return canonical_json_bytes(self._sanitize(json.loads(raw))) + b"\n"
            if path.suffix == ".jsonl":
                lines = [
                    canonical_json_bytes(self._sanitize(json.loads(line)))
                    for line in raw.splitlines()
                    if line.strip()
                ]
                return b"\n".join(lines) + (b"\n" if lines else b"")
            text = raw.decode("utf-8")
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError(f"cannot sanitize {path}") from error
        return _sanitize_text(text).encode("utf-8")

    def _sanitize(self, value: Any) -> Any:
        if isinstance(value, list):
            return [self._sanitize(item) for item in value]
        if not isinstance(value, dict):
            return _sanitize_text(value) if isinstance(value, str) else value
        normalized = {_normalized(key): key for key in value if isinstance(key, str)}
        identity_values = [
            str(value[normalized[key]])
            for key in _IDENTITY_KEYS & normalized.keys()
            if value[normalized[key]] is not None
        ]
        alias = next((self._aliases[item] for item in identity_values if item in self._aliases), None)
        if identity_values and alias is None:
            alias = f"participant-{len(set(self._aliases.values())) + 1:04d}"
        if alias is not None:
            for item in identity_values:
                self._aliases[item] = alias
        sanitized: dict[str, Any] = {}
        for key, item in value.items():
            normalized_key = _normalized(key)
            if normalized_key in _IDENTITY_KEYS:
                sanitized[key] = alias or "participant-unknown"
            elif normalized_key in _SECRET_KEYS or any(
                marker in normalized_key for marker in ("token", "cookie", "credential", "password")
            ):
                sanitized[key] = "<redacted-secret>"
            elif normalized_key in _NETWORK_KEYS or "endpoint" in normalized_key:
                sanitized[key] = "<redacted-endpoint>"
            elif normalized_key in _PATH_KEYS or (
                normalized_key.endswith("path")
                and isinstance(item, str)
                and item.startswith(("/", "\\"))
            ):
                sanitized[key] = "<redacted-path>"
            elif normalized_key in _STACK_KEYS or "stack" in normalized_key:
                sanitized[key] = "<redacted-stack>"
            else:
                sanitized[key] = self._sanitize(item)
        return sanitized


def scan_public_bytes(value: bytes) -> list[str]:
    if not isinstance(value, bytes):
        return ["PUBLIC_ARTIFACT_NOT_BYTES"]
    return [f"PUBLIC_LEAK_PATTERN_{index + 1}" for index, pattern in enumerate(_LEAK_PATTERNS) if pattern.search(value)]


def close_package(
    run_directory: Path | str,
    output_directory: Path | str,
    *,
    run_id: str,
    required_paths: Iterable[str],
) -> dict[str, Any]:
    source = Path(run_directory)
    target = Path(output_directory)
    if target.exists():
        raise FileExistsError(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    required = tuple(sorted(set(required_paths)))
    if not required or any(not _safe_relative(path) for path in required):
        raise ValueError("required_paths must contain safe relative paths")
    temporary = Path(tempfile.mkdtemp(prefix=f".{target.name}.", dir=target.parent))
    try:
        sanitizer = EvidenceSanitizer()
        missing: list[str] = []
        files: list[dict[str, Any]] = []
        raw_inventory: list[dict[str, Any]] = []
        sanitized_inventory: list[dict[str, Any]] = []
        packaged_paths: list[Path] = []
        sanitization_failed = False
        for relative in required:
            raw_path = source / relative
            if not raw_path.is_file():
                missing.append(relative)
                continue
            raw = raw_path.read_bytes()
            raw_entry = {"path": relative, "sha256": _sha256(raw), "bytes": len(raw)}
            raw_inventory.append(raw_entry)
            restricted = temporary / "restricted" / relative
            _write_fsynced(restricted, raw)
            packaged_paths.append(restricted)
            files.append(
                _file_entry(
                    f"restricted/{relative}",
                    raw,
                    "RESTRICTED_RAW_DIAGNOSTICS",
                )
            )
            try:
                sanitized = sanitizer.sanitize_bytes(Path(relative), raw)
            except ValueError:
                sanitization_failed = True
                continue
            if scan_public_bytes(sanitized):
                sanitization_failed = True
                continue
            packaged = temporary / "sanitized" / relative
            _write_fsynced(packaged, sanitized)
            packaged_paths.append(packaged)
            sanitized_entry = {
                "path": f"sanitized/{relative}",
                "sha256": _sha256(sanitized),
                "bytes": len(sanitized),
            }
            sanitized_inventory.append(sanitized_entry)
            files.append(
                _file_entry(
                    sanitized_entry["path"],
                    sanitized,
                    "SANITIZED_EVIDENCE_BUNDLE",
                )
            )

        raw_digest = document_digest(raw_inventory)
        sanitized_digest = document_digest(sanitized_inventory)
        status = "COMPLETE"
        reasons: list[str] = []
        if missing:
            status = "INCOMPLETE"
            reasons.append("PACKAGE_FILE_MISSING")
        if sanitization_failed:
            status = "INCOMPLETE"
            reasons.append("PACKAGE_SANITIZATION_FAILED")
            missing.append("public/sanitization-pass")
        summary = (
            "# Loot of Legends V2 Evidence Summary\n\n"
            f"- run: `{run_id}`\n"
            f"- package: `{status}`\n"
            f"- raw bundle digest: `{raw_digest}`\n"
            f"- sanitized bundle digest: `{sanitized_digest}`\n"
        ).encode()
        if scan_public_bytes(summary):
            raise ValueError("run_id crosses the public sanitization boundary")
        summary_path = temporary / "public" / "summary.md"
        _write_fsynced(summary_path, summary)
        packaged_paths.append(summary_path)
        files.append(_file_entry("public/summary.md", summary, "PUBLIC_CLAIM_SUMMARY"))
        checksum_bytes = b"".join(
            f"{_sha256(path.read_bytes())}  {path.relative_to(temporary).as_posix()}\n".encode()
            for path in sorted(packaged_paths)
        )
        checksum_path = temporary / "checksums" / "SHA256SUMS"
        _write_fsynced(checksum_path, checksum_bytes)
        files.append(
            _file_entry(
                "checksums/SHA256SUMS",
                checksum_bytes,
                "SANITIZED_EVIDENCE_BUNDLE",
            )
        )
        package = {
            "schemaVersion": 1,
            "runId": run_id,
            "packageStatus": status,
            "reasonCodes": reasons,
            "missingPaths": sorted(missing),
            "files": files,
            "rawBundleDigest": raw_digest,
            "sanitizedBundleDigest": sanitized_digest,
            "inventoryDigest": document_digest(files),
        }
        errors = validate_document("evidence-package", package)
        if errors:
            raise ValueError("evidence package is invalid: " + "; ".join(errors))
        _write_fsynced(
            temporary / "manifest" / "evidence-package.json",
            canonical_json_bytes(package) + b"\n",
        )
        os.replace(temporary, target)
        _fsync_directory(target.parent)
        return package
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def verify_package(output_directory: Path | str) -> dict[str, Any]:
    root = Path(output_directory)
    manifest = root / "manifest" / "evidence-package.json"
    value = json.loads(manifest.read_bytes())
    if not isinstance(value, dict):
        raise ValueError("evidence package manifest must be an object")
    errors = validate_document("evidence-package", value)
    if errors:
        raise ValueError("evidence package manifest is invalid: " + "; ".join(errors))
    if value.get("inventoryDigest") != document_digest(value["files"]):
        raise ValueError("evidence package inventory digest is invalid")
    raw_inventory = [
        {
            "path": entry["path"].removeprefix("restricted/"),
            "sha256": entry["sha256"],
            "bytes": entry["bytes"],
        }
        for entry in value["files"]
        if entry["retentionClass"] == "RESTRICTED_RAW_DIAGNOSTICS"
    ]
    sanitized_inventory = [
        {"path": entry["path"], "sha256": entry["sha256"], "bytes": entry["bytes"]}
        for entry in value["files"]
        if entry["path"].startswith("sanitized/")
    ]
    if value.get("rawBundleDigest") != document_digest(raw_inventory):
        raise ValueError("evidence package raw bundle digest is invalid")
    if value.get("sanitizedBundleDigest") != document_digest(sanitized_inventory):
        raise ValueError("evidence package sanitized bundle digest is invalid")
    verified = json.loads(json.dumps(value))
    missing: list[str] = []
    mismatched = False
    for entry in verified["files"]:
        path = root / entry["path"]
        if not path.is_file():
            missing.append(entry["path"])
            continue
        observed = _sha256(path.read_bytes())
        entry["observedSha256"] = observed
        mismatched |= observed != entry["sha256"]
    checksum_path = root / "checksums" / "SHA256SUMS"
    if not mismatched and checksum_path.is_file():
        expected_checksums = {
            entry["path"]: entry["sha256"]
            for entry in value["files"]
            if entry["path"] != "checksums/SHA256SUMS"
        }
        if _parse_checksums(checksum_path.read_bytes()) != expected_checksums:
            raise ValueError("evidence package checksum inventory is invalid")
    if mismatched:
        verified["packageStatus"] = "CORRUPT"
        verified["reasonCodes"] = ["PACKAGE_CHECKSUM_MISMATCH"]
        verified["missingPaths"] = []
    elif missing:
        verified["packageStatus"] = "INCOMPLETE"
        verified["reasonCodes"] = ["PACKAGE_FILE_MISSING"]
        verified["missingPaths"] = sorted(missing)
    errors = validate_document("evidence-package", verified)
    if errors:
        raise ValueError("verified evidence package is invalid: " + "; ".join(errors))
    return verified


def _file_entry(path: str, value: bytes, retention: str) -> dict[str, Any]:
    digest = _sha256(value)
    return {
        "path": path,
        "sha256": digest,
        "observedSha256": digest,
        "bytes": len(value),
        "retentionClass": retention,
    }


def _sanitize_text(value: str) -> str:
    encoded = value.encode("utf-8")
    for pattern in _LEAK_PATTERNS:
        encoded = pattern.sub(b"<redacted>", encoded)
    return encoded.decode("utf-8")


def _normalized(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.lower())


def _safe_relative(value: str) -> bool:
    path = Path(value)
    return bool(value) and not path.is_absolute() and ".." not in path.parts and "\\" not in value


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _write_fsynced(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(value)
        output.flush()
        os.fsync(output.fileno())


def _parse_checksums(value: bytes) -> dict[str, str]:
    checksums: dict[str, str] = {}
    try:
        lines = value.decode("ascii").splitlines()
    except UnicodeDecodeError as error:
        raise ValueError("SHA256SUMS must be ASCII") from error
    for line in lines:
        if len(line) < 67 or line[64:66] != "  ":
            raise ValueError("SHA256SUMS line is malformed")
        digest, path = line[:64], line[66:]
        if not re.fullmatch(r"[0-9a-f]{64}", digest) or not _safe_relative(path):
            raise ValueError("SHA256SUMS line is malformed")
        if path in checksums:
            raise ValueError("SHA256SUMS path is duplicated")
        checksums[path] = digest
    return checksums


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
