"""Durable, no-clobber writers for raw facts and immutable manifests."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path
from typing import Any

from .contracts import canonical_json_bytes


def write_json_atomic(path: Path | str, value: object, *, overwrite: bool = False) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    if not overwrite and target.exists():
        raise FileExistsError(target)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{target.name}.",
            suffix=".tmp",
            dir=target.parent,
            delete=False,
        ) as output:
            temporary = Path(output.name)
            output.write(canonical_json_bytes(value) + b"\n")
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, target)
        temporary = None
        _fsync_directory(target.parent)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


class JsonlFactWriter:
    def __init__(
        self,
        path: Path | str,
        *,
        stream_id: str,
        max_record_bytes: int = 65_536,
    ) -> None:
        if not stream_id or len(stream_id) > 128:
            raise ValueError("stream_id must contain 1..128 characters")
        if max_record_bytes < 256:
            raise ValueError("max_record_bytes must be at least 256")
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._stream_id = stream_id
        self._max_record_bytes = max_record_bytes
        self._sequence = 0
        self._descriptor = os.open(
            self.path,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_APPEND,
            0o600,
        )

    def append(self, fact: dict[str, Any]) -> int:
        if self._descriptor < 0:
            raise ValueError("fact writer is closed")
        if not isinstance(fact, dict):
            raise TypeError("fact must be an object")
        reserved = {"sequence", "streamId"} & fact.keys()
        if reserved:
            raise ValueError(f"fact contains reserved fields: {sorted(reserved)!r}")
        sequence = self._sequence + 1
        encoded = canonical_json_bytes(
            {"sequence": sequence, "streamId": self._stream_id, **fact}
        ) + b"\n"
        if len(encoded) > self._max_record_bytes:
            raise ValueError("fact exceeds max_record_bytes")
        written = os.write(self._descriptor, encoded)
        if written != len(encoded):
            raise OSError(f"short append: wrote {written} of {len(encoded)} bytes")
        self._sequence = sequence
        return sequence

    def flush(self) -> None:
        if self._descriptor < 0:
            raise ValueError("fact writer is closed")
        os.fsync(self._descriptor)

    def close(self) -> None:
        if self._descriptor < 0:
            return
        try:
            self.flush()
        finally:
            os.close(self._descriptor)
            self._descriptor = -1
        _fsync_directory(self.path.parent)

    def __enter__(self) -> JsonlFactWriter:
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()


def _fsync_directory(directory: Path) -> None:
    descriptor = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
