#!/usr/bin/env python3

import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path


GOLDEN = (
    Path(__file__).resolve().parents[2] / "contracts" / "settlement" / "golden"
)
HEADER_SIZE = 56
RECORD_OVERHEAD = 60


def load_hex(name: str) -> bytes:
    return bytes.fromhex((GOLDEN / name).read_text(encoding="ascii"))


def records(data: bytes) -> list[tuple[int, int, bytes]]:
    parsed = []
    offset = 0
    while offset < len(data):
        assert len(data) - offset >= RECORD_OVERHEAD
        magic, version, kind, record_length, sequence, payload_length = struct.unpack(
            ">IHHIQI", data[offset : offset + 24]
        )
        assert magic == 0x4C4F4F32
        assert version == 1
        assert kind in (1, 2, 3)
        assert record_length == RECORD_OVERHEAD + payload_length
        assert offset + record_length <= len(data)
        payload_hash = data[offset + 24 : offset + HEADER_SIZE]
        payload = data[
            offset + HEADER_SIZE : offset + HEADER_SIZE + payload_length
        ]
        expected_crc = struct.unpack(
            ">I", data[offset + record_length - 4 : offset + record_length]
        )[0]
        assert hashlib.sha256(payload).digest() == payload_hash
        assert zlib.crc32(data[offset : offset + record_length - 4]) == expected_crc
        parsed.append((kind, sequence, payload))
        offset += record_length
    return parsed


def main() -> int:
    manifest = json.loads(
        (GOLDEN / "outbox-journal-v1-manifest.json").read_text(encoding="utf-8")
    )
    fixtures = {entry["name"]: entry for entry in manifest["fixtures"]}
    decoded = {name: load_hex(entry["file"]) for name, entry in fixtures.items()}

    for name, data in decoded.items():
        assert len(data) == fixtures[name]["decodedBytes"]
        assert hashlib.sha256(data).hexdigest() == fixtures[name]["sha256"]

    valid_records = records(decoded["valid"])
    assert [record[:2] for record in valid_records] == [(1, 1), (1, 2), (2, 3)]
    commit = valid_records[2][2]
    assert commit[:16].hex() == manifest["batchId"]
    assert struct.unpack(">H", commit[16:18])[0] == 2
    assert struct.unpack(">Q", commit[18:26])[0] == 1
    assert commit[26:58] == hashlib.sha256(valid_records[0][2]).digest()
    assert struct.unpack(">Q", commit[58:66])[0] == 2
    assert commit[66:98] == hashlib.sha256(valid_records[1][2]).digest()

    assert decoded["partial"] == decoded["valid"][:-17]
    assert decoded["corrupt"][: len(decoded["valid"])] == decoded["valid"]
    assert decoded["corrupt"][:-1] == decoded["retired"][:-1]
    assert decoded["corrupt"][-1] == decoded["retired"][-1] ^ 1

    retired_records = records(decoded["retired"])
    assert [record[:2] for record in retired_records] == [
        (1, 1),
        (1, 2),
        (2, 3),
        (3, 4),
    ]
    assert retired_records[3][2] == bytes.fromhex(manifest["batchId"]) + struct.pack(
        ">Q", 3
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError:
        sys.exit(1)
