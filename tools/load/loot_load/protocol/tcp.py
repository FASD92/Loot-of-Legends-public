"""Strict TCP wire codec and one-reader-task async client."""

from __future__ import annotations

import asyncio
import contextlib
import struct
from dataclasses import dataclass
from typing import Any

from .errors import ProtocolError


PROTOCOL_MAJOR = 1
MAX_FRAME_BYTES = 64 * 1024


@dataclass(frozen=True)
class TcpMessage:
    name: str
    fields: dict[str, Any]


_MESSAGE_NAMES = {
    1: "AuthenticateGameSession",
    2: "Welcome",
    3: "AuthenticationRejected",
    4: "SessionReplaced",
    5: "LobbyEntrySnapshot",
    6: "LobbyRoomListUpdate",
    7: "CreateRoom",
    8: "JoinRoom",
    9: "LeaveRoom",
    10: "SetReady",
    11: "KickRoomMember",
    12: "RoomCommandResponse",
    13: "RoomDetailProjection",
    14: "HostStartRequest",
    15: "BattleCommandResponse",
    16: "ArenaLoadEntry",
    17: "ArenaLoadComplete",
    18: "ArenaGameplayStart",
    19: "ArenaLoadCancelled",
    20: "RequestRudpBindCapability",
    21: "RudpBindCapability",
    36: "FinalResult",
    37: "BattleRecoveryNotice",
}
_MESSAGE_IDS = {name: message_id for message_id, name in _MESSAGE_NAMES.items()}
_AUTH_REASONS = {
    "INVALID": 1,
    "EXPIRED": 2,
    "ALREADY_CONSUMED": 3,
    "WRONG_AUDIENCE": 4,
    "DEPENDENCY_UNAVAILABLE": 5,
    "PRE_AUTH_COMMAND": 6,
}
_SESSION_REASONS = {"SAME_ACCOUNT_LOGIN": 1}
_ROOM_RESULTS = {
    "OK": 0,
    "INVALID_ARGUMENT": 1,
    "ALREADY_IN_ROOM": 2,
    "ROOM_NOT_FOUND": 3,
    "ROOM_CLOSED": 4,
    "ROOM_FULL": 5,
    "NOT_IN_ROOM": 6,
    "NOT_HOST": 7,
    "NOT_ENOUGH_PLAYERS": 8,
    "NOT_ALL_READY": 9,
    "INVALID_TARGET": 10,
    "ROOM_OVERLOADED": 11,
    "STALE_SESSION": 12,
}
_BATTLE_RESULTS = {
    "OK": 0,
    "INVALID_ARGUMENT": 1,
    "ROOM_NOT_FOUND": 2,
    "ROOM_NOT_OPEN": 3,
    "NOT_IN_ROOM": 4,
    "NOT_HOST": 5,
    "NOT_ENOUGH_PLAYERS": 6,
    "NOT_ALL_READY": 7,
    "START_GATE_CLOSED": 8,
    "STALE_SESSION": 9,
    "STALE_BATTLE": 10,
    "NOT_ELIGIBLE": 11,
    "OVERLOADED": 12,
}
_LOAD_CANCEL_REASONS = {"NOT_ENOUGH_READY": 1}
_FINAL_OUTCOMES = {
    "MONSTER_DEFEATED": 1,
    "COMBAT_TIMEOUT": 2,
    "CANCELLED_NO_ACTIVE_PARTICIPANTS": 3,
}
_FINAL_EXIT_STATUSES = {"TERMINAL_PRESENT": 1, "TERMINAL_EXITED": 2}
_RECOVERY_REASONS = {
    "RESULT_GENERATION_FAILED": 1,
    "SETTLEMENT_RECOVERY_PENDING": 2,
}


def encode_tcp_message(name: str, fields: dict[str, Any]) -> bytes:
    message_id = _MESSAGE_IDS.get(name)
    if message_id is None:
        raise ProtocolError(f"unknown TCP message {name!r}")
    payload = bytearray(struct.pack(">BI", PROTOCOL_MAJOR, message_id))
    append = payload.extend

    if name == "AuthenticateGameSession":
        credential = _credential(fields.get("credential"))
        append(_u64(fields, "requestId"))
        append(struct.pack(">H", len(credential)))
        append(credential)
    elif name == "Welcome":
        append(_u64(fields, "requestId"))
        append(_u64(fields, "sessionId"))
        append(_u64(fields, "sessionGeneration"))
        append(_uint(fields, "serverTimeUnixMillis", 64, minimum=0))
        append(_text16(fields.get("nickname"), "nickname"))
    elif name == "AuthenticationRejected":
        append(_u64(fields, "requestId"))
        append(struct.pack(">H", _enum(fields.get("reason"), _AUTH_REASONS, "reason")))
    elif name == "SessionReplaced":
        append(struct.pack(">H", _enum(fields.get("reason"), _SESSION_REASONS, "reason")))
    elif name == "LobbyEntrySnapshot":
        append(_u64(fields, "sessionId"))
        append(_u64(fields, "sessionGeneration"))
        append(_text16(fields.get("nickname"), "nickname"))
        append(_room_list(fields.get("rooms")))
    elif name == "LobbyRoomListUpdate":
        append(_room_list(fields.get("rooms")))
    elif name == "CreateRoom":
        append(_u64(fields, "requestId"))
        append(_title8(fields.get("title")))
        append(struct.pack(">B", _capacity(fields.get("capacity"))))
    elif name == "JoinRoom":
        append(_u64(fields, "requestId"))
        append(_u64(fields, "roomId"))
    elif name == "LeaveRoom":
        append(_u64(fields, "requestId"))
    elif name == "SetReady":
        append(_u64(fields, "requestId"))
        ready = fields.get("ready")
        if not isinstance(ready, bool):
            raise ProtocolError("ready must be boolean")
        append(struct.pack(">B", int(ready)))
    elif name == "KickRoomMember":
        append(_u64(fields, "requestId"))
        append(_u64(fields, "targetSessionId"))
        append(_u64(fields, "targetSessionGeneration"))
    elif name == "RoomCommandResponse":
        append(_u64(fields, "requestId"))
        append(struct.pack(">H", _enum(fields.get("resultCode"), _ROOM_RESULTS, "resultCode")))
    elif name == "RoomDetailProjection":
        append(_u64(fields, "roomId"))
        append(_title8(fields.get("title")))
        capacity = _capacity(fields.get("capacity"))
        append(struct.pack(">B", capacity))
        host_id = _positive(fields.get("hostSessionId"), "hostSessionId", 64)
        host_generation = _positive(
            fields.get("hostSessionGeneration"), "hostSessionGeneration", 64
        )
        append(struct.pack(">QQ", host_id, host_generation))
        members = fields.get("members")
        if not isinstance(members, list) or not 1 <= len(members) <= capacity:
            raise ProtocolError("members must fit room capacity")
        if not any(
            isinstance(member, dict)
            and member.get("sessionId") == host_id
            and member.get("sessionGeneration") == host_generation
            for member in members
        ):
            raise ProtocolError("host must be present in members")
        append(struct.pack(">B", len(members)))
        for member in members:
            if not isinstance(member, dict):
                raise ProtocolError("member must be an object")
            append(_u64(member, "sessionId"))
            append(_u64(member, "sessionGeneration"))
            append(_text16(member.get("nickname"), "nickname"))
            ready = member.get("ready")
            if not isinstance(ready, bool):
                raise ProtocolError("member.ready must be boolean")
            append(struct.pack(">B", int(ready)))
    elif name in {"HostStartRequest", "RequestRudpBindCapability"}:
        append(_u64(fields, "requestId"))
    elif name == "BattleCommandResponse":
        append(_u64(fields, "requestId"))
        append(struct.pack(">H", _enum(fields.get("resultCode"), _BATTLE_RESULTS, "resultCode")))
    elif name == "ArenaLoadEntry":
        append(_u64(fields, "roomId"))
        append(_u64(fields, "battleInstanceId"))
    elif name == "ArenaLoadComplete":
        append(_u64(fields, "requestId"))
        append(_u64(fields, "roomId"))
        append(_u64(fields, "battleInstanceId"))
    elif name == "ArenaGameplayStart":
        append(_u64(fields, "roomId"))
        append(_u64(fields, "battleInstanceId"))
        participants = fields.get("participants")
        if not isinstance(participants, list) or not 2 <= len(participants) <= 10:
            raise ProtocolError("participants must contain 2..10 entries")
        session_ids: set[int] = set()
        append(struct.pack(">B", len(participants)))
        for participant in participants:
            if not isinstance(participant, dict):
                raise ProtocolError("participant must be an object")
            session_id = _positive(participant.get("sessionId"), "sessionId", 64)
            if session_id in session_ids:
                raise ProtocolError("participant sessionId is duplicated")
            session_ids.add(session_id)
            append(struct.pack(">Q", session_id))
            append(_u64(participant, "sessionGeneration"))
            append(_text16(participant.get("nickname"), "nickname"))
    elif name == "ArenaLoadCancelled":
        append(_u64(fields, "roomId"))
        append(_u64(fields, "battleInstanceId"))
        append(struct.pack(">H", _enum(fields.get("reasonCode"), _LOAD_CANCEL_REASONS, "reasonCode")))
    elif name == "RudpBindCapability":
        append(_u64(fields, "requestId"))
        if fields.get("ttlMillis") != 15000:
            raise ProtocolError("ttlMillis must be 15000")
        append(struct.pack(">I", 15000))
        capability = fields.get("capability")
        if not isinstance(capability, bytes) or len(capability) != 32 or not any(capability):
            raise ProtocolError("capability must be 32 nonzero bytes")
        append(capability)
    elif name == "FinalResult":
        append(_u64(fields, "roomId"))
        append(_u64(fields, "battleInstanceId"))
        outcome = _enum(fields.get("outcome"), _FINAL_OUTCOMES, "outcome")
        entries = fields.get("entries")
        if not isinstance(entries, list) or not 2 <= len(entries) <= 10:
            raise ProtocolError("entries must contain 2..10 results")
        append(struct.pack(">BH", outcome, len(entries)))
        sessions: set[int] = set()
        for entry in entries:
            if not isinstance(entry, dict):
                raise ProtocolError("entry must be an object")
            session_id = _positive(entry.get("sessionId"), "sessionId", 64)
            if session_id in sessions:
                raise ProtocolError("final result sessionId is duplicated")
            sessions.add(session_id)
            exit_status = _enum(
                entry.get("exitStatus"), _FINAL_EXIT_STATUSES, "exitStatus"
            )
            rank = _positive(entry.get("rank"), "rank", 32, allow_zero=True)
            is_top = entry.get("isTop")
            if not isinstance(is_top, bool):
                raise ProtocolError("isTop must be boolean")
            if (outcome == 1 and (rank == 0 or is_top != (rank == 1))) or (
                outcome != 1 and (rank != 0 or is_top)
            ):
                raise ProtocolError("rank/isTop does not match outcome")
            append(struct.pack(">Q", session_id))
            append(_text16(entry.get("nickname"), "nickname"))
            append(struct.pack(">BQIB", exit_status, _nonnegative(entry.get("finalAssetValue"), "finalAssetValue", 64), rank, int(is_top)))
    elif name == "BattleRecoveryNotice":
        append(_u64(fields, "roomId"))
        append(_u64(fields, "battleInstanceId"))
        append(struct.pack(">B", _enum(fields.get("reasonCode"), _RECOVERY_REASONS, "reasonCode")))

    if not 0 < len(payload) <= MAX_FRAME_BYTES:
        raise ProtocolError("TCP payload length is outside the boundary")
    return struct.pack(">I", len(payload)) + payload


def decode_tcp_frame(frame: bytes) -> TcpMessage:
    if not isinstance(frame, bytes) or len(frame) < 9:
        raise ProtocolError("partial TCP frame")
    payload_length = int.from_bytes(frame[:4], "big")
    if payload_length == 0 or payload_length > MAX_FRAME_BYTES:
        raise ProtocolError("invalid TCP payload length")
    if payload_length != len(frame) - 4:
        raise ProtocolError("TCP frame length mismatch")
    reader = _Reader(frame[4:])
    if reader.u8() != PROTOCOL_MAJOR:
        raise ProtocolError("unsupported TCP protocol version")
    message_id = reader.u32()
    name = _MESSAGE_NAMES.get(message_id)
    if name is None:
        raise ProtocolError(f"unknown TCP message id {message_id}")
    fields = _decode_payload(name, reader)
    reader.finish()
    message = TcpMessage(name, fields)
    if encode_tcp_message(name, fields) != frame:
        raise ProtocolError(f"non-canonical or invalid {name} frame")
    return message


def _decode_payload(name: str, reader: _Reader) -> dict[str, Any]:
    if name == "AuthenticateGameSession":
        request_id = reader.u64()
        return {"requestId": request_id, "credential": reader.text(reader.u16())}
    if name == "Welcome":
        return {
            "requestId": reader.u64(),
            "sessionId": reader.u64(),
            "sessionGeneration": reader.u64(),
            "serverTimeUnixMillis": reader.u64(),
            "nickname": reader.text(reader.u16()),
        }
    if name == "AuthenticationRejected":
        return {"requestId": reader.u64(), "reason": _enum_name(reader.u16(), _AUTH_REASONS, "reason")}
    if name == "SessionReplaced":
        return {"reason": _enum_name(reader.u16(), _SESSION_REASONS, "reason")}
    if name == "LobbyEntrySnapshot":
        return {
            "sessionId": reader.u64(),
            "sessionGeneration": reader.u64(),
            "nickname": reader.text(reader.u16()),
            "rooms": _read_rooms(reader),
        }
    if name == "LobbyRoomListUpdate":
        return {"rooms": _read_rooms(reader)}
    if name == "CreateRoom":
        return {
            "requestId": reader.u64(),
            "title": reader.text(reader.u8()),
            "capacity": reader.u8(),
        }
    if name == "JoinRoom":
        return {"requestId": reader.u64(), "roomId": reader.u64()}
    if name == "LeaveRoom":
        return {"requestId": reader.u64()}
    if name == "SetReady":
        return {"requestId": reader.u64(), "ready": _bool(reader.u8(), "ready")}
    if name == "KickRoomMember":
        return {
            "requestId": reader.u64(),
            "targetSessionId": reader.u64(),
            "targetSessionGeneration": reader.u64(),
        }
    if name == "RoomCommandResponse":
        return {"requestId": reader.u64(), "resultCode": _enum_name(reader.u16(), _ROOM_RESULTS, "resultCode")}
    if name == "RoomDetailProjection":
        room_id = reader.u64()
        title = reader.text(reader.u8())
        capacity = reader.u8()
        host_id = reader.u64()
        host_generation = reader.u64()
        members = []
        for _ in range(reader.u8()):
            members.append(
                {
                    "sessionId": reader.u64(),
                    "sessionGeneration": reader.u64(),
                    "nickname": reader.text(reader.u16()),
                    "ready": _bool(reader.u8(), "ready"),
                }
            )
        return {
            "roomId": room_id,
            "title": title,
            "capacity": capacity,
            "hostSessionId": host_id,
            "hostSessionGeneration": host_generation,
            "members": members,
        }
    if name in {"HostStartRequest", "RequestRudpBindCapability"}:
        return {"requestId": reader.u64()}
    if name == "BattleCommandResponse":
        return {"requestId": reader.u64(), "resultCode": _enum_name(reader.u16(), _BATTLE_RESULTS, "resultCode")}
    if name == "ArenaLoadEntry":
        return {"roomId": reader.u64(), "battleInstanceId": reader.u64()}
    if name == "ArenaLoadComplete":
        return {
            "requestId": reader.u64(),
            "roomId": reader.u64(),
            "battleInstanceId": reader.u64(),
        }
    if name == "ArenaGameplayStart":
        room_id = reader.u64()
        battle_id = reader.u64()
        participants = []
        for _ in range(reader.u8()):
            participants.append(
                {
                    "sessionId": reader.u64(),
                    "sessionGeneration": reader.u64(),
                    "nickname": reader.text(reader.u16()),
                }
            )
        return {"roomId": room_id, "battleInstanceId": battle_id, "participants": participants}
    if name == "ArenaLoadCancelled":
        return {
            "roomId": reader.u64(),
            "battleInstanceId": reader.u64(),
            "reasonCode": _enum_name(reader.u16(), _LOAD_CANCEL_REASONS, "reasonCode"),
        }
    if name == "RudpBindCapability":
        return {
            "requestId": reader.u64(),
            "ttlMillis": reader.u32(),
            "capability": reader.bytes(32),
        }
    if name == "FinalResult":
        room_id = reader.u64()
        battle_id = reader.u64()
        outcome = _enum_name(reader.u8(), _FINAL_OUTCOMES, "outcome")
        entries = []
        for _ in range(reader.u16()):
            entries.append(
                {
                    "sessionId": reader.u64(),
                    "nickname": reader.text(reader.u16()),
                    "exitStatus": _enum_name(reader.u8(), _FINAL_EXIT_STATUSES, "exitStatus"),
                    "finalAssetValue": reader.u64(),
                    "rank": reader.u32(),
                    "isTop": _bool(reader.u8(), "isTop"),
                }
            )
        return {"roomId": room_id, "battleInstanceId": battle_id, "outcome": outcome, "entries": entries}
    if name == "BattleRecoveryNotice":
        return {
            "roomId": reader.u64(),
            "battleInstanceId": reader.u64(),
            "reasonCode": _enum_name(reader.u8(), _RECOVERY_REASONS, "reasonCode"),
        }
    raise ProtocolError(f"decoder missing for {name}")


class TcpProtocolClient:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self._reader = reader
        self._writer = writer
        self._write_lock = asyncio.Lock()
        self._pending: dict[int, tuple[frozenset[str], asyncio.Future[TcpMessage]]] = {}
        self._events: asyncio.Queue[TcpMessage] = asyncio.Queue(maxsize=1024)
        self._next_request_id = 1
        self._closed = False
        self._receive_error: Exception | None = None
        self._receive_task = asyncio.create_task(self._receive_loop(), name="loot-load-tcp-receive")

    @classmethod
    async def connect(
        cls, host: str, port: int, *, ssl: Any = None, timeout_seconds: float = 5.0
    ) -> TcpProtocolClient:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port, ssl=ssl), timeout=timeout_seconds
        )
        return cls(reader, writer)

    @property
    def receive_task(self) -> asyncio.Task[None]:
        return self._receive_task

    async def request(
        self,
        name: str,
        fields: dict[str, Any],
        *,
        expected: str | set[str] | frozenset[str],
        timeout_seconds: float = 5.0,
    ) -> TcpMessage:
        if "requestId" in fields:
            raise ProtocolError("requestId is owned by TcpProtocolClient")
        request_id = self._take_request_id()
        future = asyncio.get_running_loop().create_future()
        expected_names = frozenset({expected}) if isinstance(expected, str) else frozenset(expected)
        if not expected_names:
            raise ProtocolError("expected response set must not be empty")
        self._pending[request_id] = (expected_names, future)
        try:
            await self.send(name, {**fields, "requestId": request_id})
            return await asyncio.wait_for(future, timeout=timeout_seconds)
        finally:
            self._pending.pop(request_id, None)

    async def send(self, name: str, fields: dict[str, Any]) -> None:
        if self._closed:
            raise ProtocolError("TCP client is closed")
        if self._receive_error is not None:
            raise ProtocolError("TCP receive task failed") from self._receive_error
        frame = encode_tcp_message(name, fields)
        async with self._write_lock:
            self._writer.write(frame)
            await self._writer.drain()

    async def next_event(self, *, timeout_seconds: float = 5.0) -> TcpMessage:
        if self._receive_error is not None:
            raise ProtocolError("TCP receive task failed") from self._receive_error
        return await asyncio.wait_for(self._events.get(), timeout=timeout_seconds)

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        error = ProtocolError("TCP client closed")
        for _, future in self._pending.values():
            if not future.done():
                future.set_exception(error)
        self._writer.close()
        with contextlib.suppress(Exception):
            await self._writer.wait_closed()
        self._receive_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await self._receive_task

    async def _receive_loop(self) -> None:
        try:
            while not self._closed:
                header = await self._reader.readexactly(4)
                length = int.from_bytes(header, "big")
                if length == 0 or length > MAX_FRAME_BYTES:
                    raise ProtocolError("invalid inbound TCP frame length")
                message = decode_tcp_frame(header + await self._reader.readexactly(length))
                request_id = message.fields.get("requestId")
                pending = self._pending.get(request_id) if isinstance(request_id, int) else None
                if pending is not None:
                    expected, future = pending
                    if future.done():
                        continue
                    if message.name not in expected:
                        future.set_exception(
                            ProtocolError(
                                f"request {request_id} expected {sorted(expected)!r}, got {message.name}"
                            )
                        )
                    else:
                        future.set_result(message)
                else:
                    try:
                        self._events.put_nowait(message)
                    except asyncio.QueueFull as exc:
                        raise ProtocolError("TCP event queue is full") from exc
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self._receive_error = exc
            for _, future in self._pending.values():
                if not future.done():
                    future.set_exception(exc)

    def _take_request_id(self) -> int:
        request_id = self._next_request_id
        self._next_request_id = 1 if request_id == (1 << 64) - 1 else request_id + 1
        if request_id in self._pending:
            raise ProtocolError("requestId space is exhausted")
        return request_id


class _Reader:
    def __init__(self, data: bytes) -> None:
        self._data = data
        self._offset = 0

    def bytes(self, count: int) -> bytes:
        if count < 0 or self._offset + count > len(self._data):
            raise ProtocolError("truncated TCP payload")
        value = self._data[self._offset : self._offset + count]
        self._offset += count
        return value

    def uint(self, count: int) -> int:
        return int.from_bytes(self.bytes(count), "big")

    def u8(self) -> int:
        return self.uint(1)

    def u16(self) -> int:
        return self.uint(2)

    def u32(self) -> int:
        return self.uint(4)

    def u64(self) -> int:
        return self.uint(8)

    def text(self, count: int) -> str:
        try:
            return self.bytes(count).decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise ProtocolError("invalid UTF-8") from exc

    def finish(self) -> None:
        if self._offset != len(self._data):
            raise ProtocolError("trailing TCP payload bytes")


def _read_rooms(reader: _Reader) -> list[dict[str, Any]]:
    rooms = []
    for _ in range(reader.u16()):
        rooms.append(
            {
                "roomId": reader.u64(),
                "title": reader.text(reader.u8()),
                "memberCount": reader.u8(),
                "capacity": reader.u8(),
            }
        )
    return rooms


def _room_list(value: object) -> bytes:
    if not isinstance(value, list) or len(value) > 65535:
        raise ProtocolError("rooms must be an array with at most 65535 entries")
    encoded = bytearray(struct.pack(">H", len(value)))
    for room in value:
        if not isinstance(room, dict):
            raise ProtocolError("room summary must be an object")
        capacity = _capacity(room.get("capacity"))
        members = _positive(room.get("memberCount"), "memberCount", 8)
        if members > capacity:
            raise ProtocolError("memberCount exceeds capacity")
        encoded.extend(_u64(room, "roomId"))
        encoded.extend(_title8(room.get("title")))
        encoded.extend(struct.pack(">BB", members, capacity))
    return bytes(encoded)


def _title8(value: object) -> bytes:
    if not isinstance(value, str):
        raise ProtocolError("title must be text")
    normalized = value.strip(" \t\n\r\f\v")
    encoded = normalized.encode("utf-8")
    if not 1 <= len(encoded) <= 48 or any(
        ord(character) <= 0x1F or 0x7F <= ord(character) <= 0x9F
        for character in normalized
    ):
        raise ProtocolError("title violates the bounded UTF-8 contract")
    return struct.pack(">B", len(encoded)) + encoded


def _text16(value: object, field: str) -> bytes:
    if not isinstance(value, str):
        raise ProtocolError(f"{field} must be text")
    encoded = value.encode("utf-8")
    if not 1 <= len(encoded) <= 65535:
        raise ProtocolError(f"{field} length is invalid")
    return struct.pack(">H", len(encoded)) + encoded


def _credential(value: object) -> bytes:
    if not isinstance(value, str) or len(value) != 43 or any(
        not (character.isascii() and (character.isalnum() or character in "-_"))
        for character in value
    ):
        raise ProtocolError("credential must be 43 base64url characters")
    return value.encode("ascii")


def _u64(fields: dict[str, Any], field: str) -> bytes:
    return struct.pack(">Q", _positive(fields.get(field), field, 64))


def _positive(value: object, field: str, bits: int, *, allow_zero: bool = False) -> int:
    minimum = 0 if allow_zero else 1
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value < 1 << bits:
        raise ProtocolError(f"{field} must be uint{bits}{' including zero' if allow_zero else ' nonzero'}")
    return value


def _nonnegative(value: object, field: str, bits: int) -> int:
    return _positive(value, field, bits, allow_zero=True)


def _uint(fields: dict[str, Any], field: str, bits: int, *, minimum: int) -> bytes:
    value = _positive(fields.get(field), field, bits, allow_zero=minimum == 0)
    return value.to_bytes(bits // 8, "big")


def _capacity(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 2 <= value <= 10:
        raise ProtocolError("capacity must be 2..10")
    return value


def _enum(value: object, values: dict[str, int], field: str) -> int:
    if isinstance(value, str) and value in values:
        return values[value]
    if isinstance(value, int) and not isinstance(value, bool) and value in values.values():
        return value
    raise ProtocolError(f"unknown {field} {value!r}")


def _enum_name(value: int, values: dict[str, int], field: str) -> str:
    for name, number in values.items():
        if number == value:
            return name
    raise ProtocolError(f"unknown {field} {value}")


def _bool(value: int, field: str) -> bool:
    if value not in (0, 1):
        raise ProtocolError(f"{field} must be 0 or 1")
    return value == 1
