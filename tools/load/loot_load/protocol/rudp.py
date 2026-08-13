"""Strict RUDP codec, ACK window, retries, and one-reader-task peer."""

from __future__ import annotations

import asyncio
import contextlib
import socket
import struct
import time
import zlib
from dataclasses import dataclass
from typing import Any

from .errors import ProtocolError


MAGIC = b"LOL2"
PROTOCOL_MAJOR = 1
HEADER_BYTES = 48
MAX_DATAGRAM_BYTES = 1200
_HEADER = struct.Struct(">4sBBHQQIIIIHHI")
_MESSAGE_IDS = {
    "AckOnly": 0,
    "RudpBindHello": 22,
    "RudpBindAccepted": 23,
    "RudpHeartbeat": 24,
    "MoveIntent": 25,
    "StateSnapshot": 26,
    "AttackIntent": 27,
    "AttackTerminalResult": 28,
    "MonsterSpawned": 29,
    "CombatTerminalEvent": 30,
    "MonsterStateSnapshot": 31,
    "ClaimLootIntent": 32,
    "ClaimLootTerminalResult": 33,
    "DropSpawned": 34,
    "DropStateSnapshot": 35,
}
_MESSAGE_NAMES = {value: key for key, value in _MESSAGE_IDS.items()}
_FLAGS = {
    "AckOnly": 2,
    "RudpBindHello": 1,
    "RudpBindAccepted": 1,
    "RudpHeartbeat": 4,
    "MoveIntent": 0,
    "StateSnapshot": 0,
    "AttackIntent": 1,
    "AttackTerminalResult": 1,
    "MonsterSpawned": 1,
    "CombatTerminalEvent": 1,
    "MonsterStateSnapshot": 0,
    "ClaimLootIntent": 1,
    "ClaimLootTerminalResult": 1,
    "DropSpawned": 1,
    "DropStateSnapshot": 0,
}
_ATTACK_RESULTS = {
    "OK": 0,
    "NOT_ELIGIBLE": 1,
    "STALE_SESSION": 2,
    "STALE_BATTLE": 3,
    "INVALID_TARGET": 4,
    "OUT_OF_RANGE": 5,
    "COOLDOWN": 6,
    "OVERLOADED": 7,
    "COMMAND_CONFLICT": 8,
    "TERMINAL_ALREADY_DECIDED": 9,
}
_COMBAT_OUTCOMES = {"NONE": 0, "MONSTER_DEFEATED": 1, "COMBAT_TIMEOUT": 2}
_MONSTER_STATES = {"ALIVE": 0, "DYING": 1, "DEAD": 2, "TIMED_OUT": 3}
_EVENT_STREAMS = {"COMBAT_LIFECYCLE": 1, "LOOT_LIFECYCLE": 2}
_LOOT_RESULTS = {
    "OK": 0,
    "NOT_ELIGIBLE": 1,
    "STALE_SESSION": 2,
    "STALE_BATTLE": 3,
    "INVALID_DROP": 4,
    "UNKNOWN_DROP": 5,
    "OUT_OF_RANGE": 6,
    "ALREADY_CLAIMED": 7,
    "OVERLOADED": 8,
    "COMMAND_CONFLICT": 9,
    "CATALOG_REJECTED": 10,
    "RESOLUTION_CLOSED": 11,
}
_LOOT_RESOLUTION_STATES = {"NOT_STARTED": 0, "OPEN": 1, "RESOLVED": 2}
_LOOT_DROP_STATES = {"AVAILABLE": 0, "CLAIMED": 1, "UNCLAIMED": 2}


@dataclass(frozen=True)
class RudpMessage:
    name: str
    header: dict[str, int]
    fields: dict[str, Any]


def encode_rudp_message(name: str, header: dict[str, int], fields: dict[str, Any]) -> bytes:
    message_id = _MESSAGE_IDS.get(name)
    if message_id is None:
        raise ProtocolError(f"unknown RUDP message {name!r}")
    if header.get("messageId") != message_id or header.get("flag") != _FLAGS[name]:
        raise ProtocolError(f"RUDP header does not match {name}")
    payload = _encode_payload(name, fields)
    _validate_epoch(name, header.get("transportEpoch"))
    return encode_rudp_datagram(header, payload)


def encode_rudp_datagram(header: dict[str, int], payload: bytes) -> bytes:
    if not isinstance(payload, bytes):
        raise ProtocolError("RUDP payload must be bytes")
    flag = _uint(header.get("flag"), "flag", 8)
    session_id = _positive(header.get("sessionId"), "sessionId", 64)
    generation = _positive(header.get("sessionGeneration"), "sessionGeneration", 64)
    epoch = _uint(header.get("transportEpoch"), "transportEpoch", 32)
    sequence = _positive(header.get("sequence"), "sequence", 32)
    ack = _uint(header.get("ack"), "ack", 32)
    ack_bits = _uint(header.get("ackBits"), "ackBits", 32)
    message_id = _uint(header.get("messageId"), "messageId", 16)
    if flag not in {0, 1, 2, 4}:
        raise ProtocolError("invalid RUDP flag")
    if flag in {0, 1} and message_id == 0:
        raise ProtocolError("data RUDP datagram requires messageId")
    if flag == 2 and (message_id != 0 or payload):
        raise ProtocolError("ACK_ONLY must have empty payload and messageId 0")
    if flag == 4 and (message_id != 24 or payload):
        raise ProtocolError("heartbeat must have empty payload and messageId 24")
    if HEADER_BYTES + len(payload) > MAX_DATAGRAM_BYTES:
        raise ProtocolError("RUDP datagram exceeds 1200 bytes")
    datagram = bytearray(
        _HEADER.pack(
            MAGIC,
            PROTOCOL_MAJOR,
            flag,
            HEADER_BYTES,
            session_id,
            generation,
            epoch,
            sequence,
            ack,
            ack_bits,
            message_id,
            len(payload),
            0,
        )
    )
    datagram.extend(payload)
    checksum = zlib.crc32(datagram) & 0xFFFFFFFF
    datagram[44:48] = checksum.to_bytes(4, "big")
    return bytes(datagram)


def decode_rudp_datagram(datagram: bytes) -> RudpMessage:
    if not isinstance(datagram, bytes) or not HEADER_BYTES <= len(datagram) <= MAX_DATAGRAM_BYTES:
        raise ProtocolError("RUDP datagram length is invalid")
    (
        magic,
        version,
        flag,
        header_bytes,
        session_id,
        generation,
        epoch,
        sequence,
        ack,
        ack_bits,
        message_id,
        payload_bytes,
        checksum,
    ) = _HEADER.unpack_from(datagram)
    if magic != MAGIC or version != PROTOCOL_MAJOR or header_bytes != HEADER_BYTES:
        raise ProtocolError("RUDP magic/version/header is invalid")
    if payload_bytes != len(datagram) - HEADER_BYTES:
        raise ProtocolError("RUDP payload length mismatch")
    checksum_input = bytearray(datagram)
    checksum_input[44:48] = b"\0\0\0\0"
    if checksum != zlib.crc32(checksum_input) & 0xFFFFFFFF:
        raise ProtocolError("RUDP checksum mismatch")
    header = {
        "flag": flag,
        "sessionId": session_id,
        "sessionGeneration": generation,
        "transportEpoch": epoch,
        "sequence": sequence,
        "ack": ack,
        "ackBits": ack_bits,
        "messageId": message_id,
    }
    name = _MESSAGE_NAMES.get(message_id)
    if name is None or flag != _FLAGS[name]:
        raise ProtocolError(f"unknown RUDP message/flag {message_id}/{flag}")
    _validate_epoch(name, epoch)
    if session_id == 0 or generation == 0 or sequence == 0:
        raise ProtocolError("RUDP identity/sequence fields must be nonzero")
    payload = datagram[HEADER_BYTES:]
    fields = _decode_payload(name, payload)
    message = RudpMessage(name, header, fields)
    if encode_rudp_message(name, header, fields) != datagram:
        raise ProtocolError(f"non-canonical or invalid {name} datagram")
    return message


def _encode_payload(name: str, fields: dict[str, Any]) -> bytes:
    if name == "AckOnly" or name in {"RudpBindAccepted", "RudpHeartbeat"}:
        if fields:
            raise ProtocolError(f"{name} has no payload fields")
        return b""
    if name == "RudpBindHello":
        capability = fields.get("capability")
        if not isinstance(capability, bytes) or len(capability) != 32:
            raise ProtocolError("bind capability must be 32 bytes")
        return capability
    if name == "MoveIntent":
        desired_x = _signed(fields.get("desiredX"), "desiredX", 16, forbidden=-(1 << 15))
        desired_y = _signed(fields.get("desiredY"), "desiredY", 16, forbidden=-(1 << 15))
        if fields.get("inputFlags") != 0:
            raise ProtocolError("MoveIntent inputFlags must be zero")
        return struct.pack(
            ">QIhhH",
            _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
            _uint(fields.get("actionSequence"), "actionSequence", 32),
            desired_x,
            desired_y,
            0,
        )
    if name == "StateSnapshot":
        players = fields.get("players")
        if not isinstance(players, list) or len(players) > 10:
            raise ProtocolError("StateSnapshot players must contain at most 10 entries")
        payload = bytearray(
            struct.pack(
                ">QIIH",
                _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
                _uint(fields.get("snapshotSequence"), "snapshotSequence", 32),
                _uint(fields.get("serverTick"), "serverTick", 32),
                len(players),
            )
        )
        for player in players:
            if not isinstance(player, dict):
                raise ProtocolError("snapshot player must be an object")
            x = _bounded_position(player.get("posXMillimeter"), "posXMillimeter")
            y = _bounded_position(player.get("posYMillimeter"), "posYMillimeter")
            payload.extend(
                struct.pack(
                    ">Qii",
                    _positive(player.get("sessionId"), "sessionId", 64),
                    x,
                    y,
                )
            )
        return bytes(payload)
    if name in {"AttackIntent", "ClaimLootIntent"}:
        command_high, command_low = _identifier(fields.get("commandId"), "commandId")
        target_field = "targetHint" if name == "AttackIntent" else "dropId"
        target = fields.get(target_field)
        if name == "AttackIntent":
            target = _positive(target, target_field, 64)
        else:
            target = _uint(target, target_field, 64)
        return struct.pack(
            ">QQQQ",
            command_high,
            command_low,
            _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
            target,
        )
    if name == "AttackTerminalResult":
        high, low = _identifier(fields.get("commandId"), "commandId")
        result_code = _enum(fields.get("resultCode"), _ATTACK_RESULTS, "resultCode")
        outcome = _enum(fields.get("combatOutcome"), _COMBAT_OUTCOMES, "combatOutcome")
        hit_points = _hit_points(fields.get("remainingHitPoints"))
        if outcome == 1 and hit_points != 0:
            raise ProtocolError("defeated monster must have zero hit points")
        if fields.get("monsterId") != 1 or fields.get("rulesetVersion") != 1:
            raise ProtocolError("combat immutable fields are invalid")
        return struct.pack(
            ">QQQHQIHB",
            high,
            low,
            _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
            result_code,
            1,
            hit_points,
            1,
            outcome,
        )
    if name == "MonsterSpawned":
        high, low = _identifier(fields.get("eventId"), "eventId")
        if (
            _enum(fields.get("eventStreamKind"), _EVENT_STREAMS, "eventStreamKind") != 1
            or fields.get("eventSequence") != 1
            or fields.get("monsterId") != 1
            or fields.get("posXMillimeter") != 0
            or fields.get("posYMillimeter") != 0
            or fields.get("maximumHitPoints") != 1600
            or fields.get("rulesetVersion") != 1
        ):
            raise ProtocolError("MonsterSpawned immutable fields are invalid")
        return struct.pack(
            ">QQQBIQiiIH",
            high,
            low,
            _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
            1,
            1,
            1,
            0,
            0,
            1600,
            1,
        )
    if name == "CombatTerminalEvent":
        high, low = _identifier(fields.get("eventId"), "eventId")
        outcome = _enum(fields.get("combatOutcome"), _COMBAT_OUTCOMES, "combatOutcome")
        if (
            _enum(fields.get("eventStreamKind"), _EVENT_STREAMS, "eventStreamKind") != 1
            or fields.get("eventSequence") != 2
            or outcome not in {1, 2}
            or fields.get("monsterId") != 1
            or fields.get("rulesetVersion") != 1
        ):
            raise ProtocolError("CombatTerminalEvent immutable fields are invalid")
        return struct.pack(
            ">QQQBIBQIH",
            high,
            low,
            _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
            1,
            2,
            outcome,
            1,
            _uint(fields.get("serverTick"), "serverTick", 32),
            1,
        )
    if name == "MonsterStateSnapshot":
        state = _enum(fields.get("monsterState"), _MONSTER_STATES, "monsterState")
        hit_points = _hit_points(fields.get("hitPoints"))
        if fields.get("monsterId") != 1 or (state in {1, 2}) != (hit_points == 0):
            raise ProtocolError("monster state/hit points are inconsistent")
        if state in {0, 3} and hit_points == 0:
            raise ProtocolError("alive/timed-out monster must retain hit points")
        return struct.pack(
            ">QIIQIB",
            _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
            _positive(fields.get("snapshotSequence"), "snapshotSequence", 32),
            _uint(fields.get("serverTick"), "serverTick", 32),
            1,
            hit_points,
            state,
        )
    if name == "ClaimLootTerminalResult":
        high, low = _identifier(fields.get("commandId"), "commandId")
        result = _enum(fields.get("resultCode"), _LOOT_RESULTS, "resultCode")
        drop_id = _uint(fields.get("dropId"), "dropId", 64)
        if result == 0 and drop_id == 0:
            raise ProtocolError("successful loot result requires dropId")
        return struct.pack(
            ">QQQQH",
            high,
            low,
            _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
            drop_id,
            result,
        )
    if name == "DropSpawned":
        high, low = _identifier(fields.get("eventId"), "eventId")
        sequence = _positive(fields.get("eventSequence"), "eventSequence", 32)
        drop_id = _positive(fields.get("dropId"), "dropId", 64)
        item_id = fields.get("itemId")
        x = _bounded_position(fields.get("posXMillimeter"), "posXMillimeter")
        y = _bounded_position(fields.get("posYMillimeter"), "posYMillimeter")
        if (
            _enum(fields.get("eventStreamKind"), _EVENT_STREAMS, "eventStreamKind") != 2
            or drop_id != sequence
            or drop_id > 10
            or item_id not in {1, 2}
            or fields.get("quantity") != 1
            or fields.get("rulesetVersion") != 1
        ):
            raise ProtocolError("DropSpawned immutable fields are invalid")
        return struct.pack(
            ">QQQBIQQQiiH",
            high,
            low,
            _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
            2,
            sequence,
            drop_id,
            item_id,
            1,
            x,
            y,
            1,
        )
    if name == "DropStateSnapshot":
        state = _enum(fields.get("resolutionState"), _LOOT_RESOLUTION_STATES, "resolutionState")
        drops = fields.get("drops")
        if not isinstance(drops, list) or len(drops) > 10:
            raise ProtocolError("DropStateSnapshot drops must contain at most 10 entries")
        if (state == 0 and drops) or (state in {1, 2} and not drops):
            raise ProtocolError("drop list does not match resolution state")
        payload = bytearray(
            struct.pack(
                ">QIBH",
                _positive(fields.get("battleInstanceId"), "battleInstanceId", 64),
                _positive(fields.get("snapshotSequence"), "snapshotSequence", 32),
                state,
                len(drops),
            )
        )
        seen: set[int] = set()
        has_available = False
        for drop in drops:
            if not isinstance(drop, dict):
                raise ProtocolError("drop projection must be an object")
            drop_id = _positive(drop.get("dropId"), "dropId", 64)
            item_id = drop.get("itemId")
            drop_state = _enum(drop.get("state"), _LOOT_DROP_STATES, "state")
            owner = _uint(drop.get("ownerSessionId"), "ownerSessionId", 64)
            if (
                drop_id > 10
                or drop_id in seen
                or item_id not in {1, 2}
                or drop.get("quantity") != 1
                or (drop_state == 1) != (owner != 0)
            ):
                raise ProtocolError("drop projection is invalid")
            seen.add(drop_id)
            has_available |= drop_state == 0
            payload.extend(
                struct.pack(
                    ">QQQiiBQ",
                    drop_id,
                    item_id,
                    1,
                    _bounded_position(drop.get("posXMillimeter"), "posXMillimeter"),
                    _bounded_position(drop.get("posYMillimeter"), "posYMillimeter"),
                    drop_state,
                    owner,
                )
            )
        if (state == 1) != has_available:
            raise ProtocolError("OPEN resolution must match available drops")
        return bytes(payload)
    raise ProtocolError(f"payload encoder missing for {name}")


def _decode_payload(name: str, payload: bytes) -> dict[str, Any]:
    if name in {"AckOnly", "RudpBindAccepted", "RudpHeartbeat"}:
        if payload:
            raise ProtocolError(f"{name} payload must be empty")
        return {}
    if name == "RudpBindHello":
        if len(payload) != 32:
            raise ProtocolError("RudpBindHello payload length is invalid")
        return {"capability": payload}
    if name == "MoveIntent":
        battle, action, x, y, flags = _unpack(">QIhhH", payload)
        return {"battleInstanceId": battle, "actionSequence": action, "desiredX": x, "desiredY": y, "inputFlags": flags}
    if name == "StateSnapshot":
        if len(payload) < 18:
            raise ProtocolError("StateSnapshot payload is truncated")
        battle, snapshot, tick, count = struct.unpack_from(">QIIH", payload)
        if len(payload) != 18 + count * 16:
            raise ProtocolError("StateSnapshot player length mismatch")
        players = []
        offset = 18
        for _ in range(count):
            session_id, x, y = struct.unpack_from(">Qii", payload, offset)
            offset += 16
            players.append({"sessionId": session_id, "posXMillimeter": x, "posYMillimeter": y})
        return {"battleInstanceId": battle, "snapshotSequence": snapshot, "serverTick": tick, "players": players}
    if name in {"AttackIntent", "ClaimLootIntent"}:
        high, low, battle, target = _unpack(">QQQQ", payload)
        return {"commandId": {"high": high, "low": low}, "battleInstanceId": battle, "targetHint" if name == "AttackIntent" else "dropId": target}
    if name == "AttackTerminalResult":
        high, low, battle, result, monster, hit_points, ruleset, outcome = _unpack(">QQQHQIHB", payload)
        return {"commandId": {"high": high, "low": low}, "battleInstanceId": battle, "resultCode": _enum_name(result, _ATTACK_RESULTS, "resultCode"), "monsterId": monster, "remainingHitPoints": hit_points, "rulesetVersion": ruleset, "combatOutcome": _enum_name(outcome, _COMBAT_OUTCOMES, "combatOutcome")}
    if name == "MonsterSpawned":
        high, low, battle, stream, sequence, monster, x, y, maximum, ruleset = _unpack(">QQQBIQiiIH", payload)
        return {"eventId": {"high": high, "low": low}, "battleInstanceId": battle, "eventStreamKind": _enum_name(stream, _EVENT_STREAMS, "eventStreamKind"), "eventSequence": sequence, "monsterId": monster, "posXMillimeter": x, "posYMillimeter": y, "maximumHitPoints": maximum, "rulesetVersion": ruleset}
    if name == "CombatTerminalEvent":
        high, low, battle, stream, sequence, outcome, monster, tick, ruleset = _unpack(">QQQBIBQIH", payload)
        return {"eventId": {"high": high, "low": low}, "battleInstanceId": battle, "eventStreamKind": _enum_name(stream, _EVENT_STREAMS, "eventStreamKind"), "eventSequence": sequence, "combatOutcome": _enum_name(outcome, _COMBAT_OUTCOMES, "combatOutcome"), "monsterId": monster, "serverTick": tick, "rulesetVersion": ruleset}
    if name == "MonsterStateSnapshot":
        battle, snapshot, tick, monster, hit_points, state = _unpack(">QIIQIB", payload)
        return {"battleInstanceId": battle, "snapshotSequence": snapshot, "serverTick": tick, "monsterId": monster, "hitPoints": hit_points, "monsterState": _enum_name(state, _MONSTER_STATES, "monsterState")}
    if name == "ClaimLootTerminalResult":
        high, low, battle, drop, result = _unpack(">QQQQH", payload)
        return {"commandId": {"high": high, "low": low}, "battleInstanceId": battle, "dropId": drop, "resultCode": _enum_name(result, _LOOT_RESULTS, "resultCode")}
    if name == "DropSpawned":
        high, low, battle, stream, sequence, drop, item, quantity, x, y, ruleset = _unpack(">QQQBIQQQiiH", payload)
        return {"eventId": {"high": high, "low": low}, "battleInstanceId": battle, "eventStreamKind": _enum_name(stream, _EVENT_STREAMS, "eventStreamKind"), "eventSequence": sequence, "dropId": drop, "itemId": item, "quantity": quantity, "posXMillimeter": x, "posYMillimeter": y, "rulesetVersion": ruleset}
    if name == "DropStateSnapshot":
        if len(payload) < 15:
            raise ProtocolError("DropStateSnapshot payload is truncated")
        battle, snapshot, state, count = struct.unpack_from(">QIBH", payload)
        if len(payload) != 15 + count * 41:
            raise ProtocolError("DropStateSnapshot projection length mismatch")
        drops = []
        offset = 15
        for _ in range(count):
            drop, item, quantity, x, y, drop_state, owner = struct.unpack_from(">QQQiiBQ", payload, offset)
            offset += 41
            drops.append({"dropId": drop, "itemId": item, "quantity": quantity, "posXMillimeter": x, "posYMillimeter": y, "state": _enum_name(drop_state, _LOOT_DROP_STATES, "state"), "ownerSessionId": owner})
        return {"battleInstanceId": battle, "snapshotSequence": snapshot, "resolutionState": _enum_name(state, _LOOT_RESOLUTION_STATES, "resolutionState"), "drops": drops}
    raise ProtocolError(f"payload decoder missing for {name}")


class AckTracker:
    def __init__(self) -> None:
        self._ack = 0
        self._ack_bits = 0

    @property
    def state(self) -> tuple[int, int]:
        return self._ack, self._ack_bits

    def observe(self, sequence: int) -> str:
        if sequence == 0:
            return "INVALID_SEQUENCE"
        if self._ack == 0:
            self._ack = sequence
            return "NEWEST"
        if sequence == self._ack:
            return "DUPLICATE"
        if _is_newer(sequence, self._ack):
            distance = (sequence - self._ack) & 0xFFFFFFFF
            self._ack_bits = 0 if distance > 32 else ((0 if distance == 32 else self._ack_bits << distance) | (1 << (distance - 1))) & 0xFFFFFFFF
            self._ack = sequence
            return "NEWEST"
        distance = (self._ack - sequence) & 0xFFFFFFFF
        if distance > 32:
            return "STALE"
        bit = 1 << (distance - 1)
        if self._ack_bits & bit:
            return "DUPLICATE"
        self._ack_bits |= bit
        return "REORDERED"

    @staticmethod
    def is_acknowledged(sequence: int, ack: int, ack_bits: int) -> bool:
        if sequence == 0 or ack == 0:
            return False
        if sequence == ack:
            return True
        if _is_newer(sequence, ack):
            return False
        distance = (ack - sequence) & 0xFFFFFFFF
        return 1 <= distance <= 32 and bool(ack_bits & (1 << (distance - 1)))


@dataclass(frozen=True)
class Transmission:
    sequence: int
    attempt: int
    kind: str
    datagram: bytes


@dataclass
class _ReliableEntry:
    sequence: int
    datagram: bytes
    lane: str
    queued_at: float
    next_at: float
    delay: float = 0.2
    attempts: int = 0


class ReliableSendQueue:
    def __init__(self) -> None:
        self._entries: dict[int, _ReliableEntry] = {}
        self._application_entries = 0
        self._bytes = 0

    def enqueue(self, sequence: int, datagram: bytes, *, lane: str, now: float) -> None:
        if sequence == 0 or sequence in self._entries:
            raise ProtocolError("reliable sequence is invalid or duplicated")
        if lane not in {"APPLICATION", "CONTROL"}:
            raise ProtocolError("reliable lane is invalid")
        if not datagram or len(datagram) > MAX_DATAGRAM_BYTES:
            raise ProtocolError("reliable datagram length is invalid")
        if len(self._entries) >= 256 or self._bytes + len(datagram) > 262_144:
            raise ProtocolError("reliable queue hard cap reached")
        if lane == "APPLICATION" and self._application_entries >= 224:
            raise ProtocolError("reliable application queue hard cap reached")
        self._entries[sequence] = _ReliableEntry(sequence, datagram, lane, now, now)
        self._bytes += len(datagram)
        self._application_entries += lane == "APPLICATION"

    def poll(self, *, now: float) -> list[Transmission]:
        transmissions = []
        for entry in self._entries.values():
            if entry.attempts < 5 and now >= entry.next_at:
                entry.attempts += 1
                transmissions.append(
                    Transmission(
                        entry.sequence,
                        entry.attempts,
                        "TRANSMIT" if entry.attempts == 1 else "RETRY",
                        entry.datagram,
                    )
                )
                if entry.attempts < 5:
                    entry.next_at = now + entry.delay
                    entry.delay = min(entry.delay * 2, 1.0)
        return transmissions

    def acknowledge(self, *, ack: int, ack_bits: int) -> list[int]:
        acknowledged = [
            sequence
            for sequence, entry in self._entries.items()
            if entry.attempts > 0 and AckTracker.is_acknowledged(sequence, ack, ack_bits)
        ]
        for sequence in acknowledged:
            self._remove(sequence)
        return acknowledged

    def expire(self, *, now: float) -> list[int]:
        expired = [
            sequence
            for sequence, entry in self._entries.items()
            if now - entry.queued_at >= 5.0
        ]
        for sequence in expired:
            self._remove(sequence)
        return expired

    def _remove(self, sequence: int) -> None:
        entry = self._entries.pop(sequence)
        self._bytes -= len(entry.datagram)
        self._application_entries -= entry.lane == "APPLICATION"


@dataclass(frozen=True)
class TransportEvent:
    kind: str
    sequence: int
    attempt: int = 0


class RudpProtocolPeer:
    def __init__(
        self,
        sock: socket.socket,
        *,
        session_id: int,
        session_generation: int,
        heartbeat_interval_seconds: float,
    ) -> None:
        self._socket = sock
        self._loop = asyncio.get_running_loop()
        self._session_id = _positive(session_id, "sessionId", 64)
        self._generation = _positive(session_generation, "sessionGeneration", 64)
        self._epoch = 0
        self._next_sequence = 1
        self._ack_tracker = AckTracker()
        self._reliable = ReliableSendQueue()
        self._heartbeat_interval = heartbeat_interval_seconds
        self._last_heartbeat = time.monotonic()
        self._messages: asyncio.Queue[RudpMessage] = asyncio.Queue(maxsize=1024)
        self._transport_events: list[TransportEvent] = []
        self._bind_future: asyncio.Future[RudpMessage] | None = None
        self._closed = False
        self._fatal_error: Exception | None = None
        self._receive_task = asyncio.create_task(self._receive_loop(), name="loot-load-rudp-receive")
        self._maintenance_task = asyncio.create_task(self._maintenance_loop(), name="loot-load-rudp-maintenance")

    @classmethod
    async def open(
        cls,
        host: str,
        port: int,
        *,
        session_id: int,
        session_generation: int,
        heartbeat_interval_seconds: float = 1.0,
    ) -> RudpProtocolPeer:
        if heartbeat_interval_seconds <= 0:
            raise ValueError("heartbeat_interval_seconds must be positive")
        loop = asyncio.get_running_loop()
        addresses = await loop.getaddrinfo(host, port, type=socket.SOCK_DGRAM)
        if not addresses:
            raise OSError("RUDP peer address did not resolve")
        family, sock_type, protocol, _, address = addresses[0]
        sock = socket.socket(family, sock_type, protocol)
        sock.setblocking(False)
        try:
            await loop.sock_connect(sock, address)
        except Exception:
            sock.close()
            raise
        return cls(
            sock,
            session_id=session_id,
            session_generation=session_generation,
            heartbeat_interval_seconds=heartbeat_interval_seconds,
        )

    @property
    def receive_task(self) -> asyncio.Task[None]:
        return self._receive_task

    @property
    def transport_epoch(self) -> int:
        return self._epoch

    async def bind(self, capability: bytes, *, timeout_seconds: float = 5.0) -> RudpMessage:
        self._raise_if_failed()
        if self._epoch != 0 or self._bind_future is not None:
            raise ProtocolError("RUDP peer bind already started")
        self._bind_future = self._loop.create_future()
        await self.send_reliable(
            "RudpBindHello", {"capability": capability}, lane="CONTROL", epoch=0
        )
        try:
            return await asyncio.wait_for(self._bind_future, timeout_seconds)
        except TimeoutError as exc:
            error = ProtocolError("RUDP bind timed out")
            self._fatal_error = error
            raise error from exc
        finally:
            self._bind_future = None

    async def send_reliable(
        self,
        name: str,
        fields: dict[str, Any],
        *,
        lane: str = "APPLICATION",
        epoch: int | None = None,
    ) -> int:
        self._raise_if_failed()
        if _FLAGS.get(name) != 1:
            raise ProtocolError(f"{name} is not a reliable RUDP message")
        sequence = self._take_sequence()
        header = self._header(_MESSAGE_IDS.get(name), _FLAGS.get(name), sequence, epoch=epoch)
        datagram = encode_rudp_message(name, header, fields)
        now = time.monotonic()
        self._reliable.enqueue(sequence, datagram, lane=lane, now=now)
        await self._flush_reliable(now)
        return sequence

    async def send_unreliable(self, name: str, fields: dict[str, Any]) -> int:
        self._raise_if_failed()
        if _FLAGS.get(name) != 0:
            raise ProtocolError(f"{name} is not an unreliable RUDP message")
        sequence = self._take_sequence()
        header = self._header(_MESSAGE_IDS.get(name), _FLAGS.get(name), sequence)
        await self._loop.sock_sendall(self._socket, encode_rudp_message(name, header, fields))
        self._record(TransportEvent("UNRELIABLE_SEND", sequence, 1))
        return sequence

    async def next_message(self, *, timeout_seconds: float = 5.0) -> RudpMessage:
        self._raise_if_failed()
        return await asyncio.wait_for(self._messages.get(), timeout_seconds)

    def drain_transport_events(self) -> list[TransportEvent]:
        events, self._transport_events = self._transport_events, []
        return events

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        for task in (self._receive_task, self._maintenance_task):
            task.cancel()
        for task in (self._receive_task, self._maintenance_task):
            with contextlib.suppress(asyncio.CancelledError):
                await task
        self._socket.close()

    async def _receive_loop(self) -> None:
        try:
            while not self._closed:
                message = decode_rudp_datagram(
                    await self._loop.sock_recv(self._socket, MAX_DATAGRAM_BYTES + 1)
                )
                header = message.header
                if (
                    header["sessionId"] != self._session_id
                    or header["sessionGeneration"] != self._generation
                ):
                    raise ProtocolError("RUDP peer identity mismatch")
                if message.name == "RudpBindAccepted":
                    if self._epoch != 0:
                        raise ProtocolError("duplicate RUDP bind acceptance")
                    self._epoch = header["transportEpoch"]
                elif header["transportEpoch"] != self._epoch:
                    raise ProtocolError("RUDP transport epoch mismatch")

                for sequence in self._reliable.acknowledge(
                    ack=header["ack"], ack_bits=header["ackBits"]
                ):
                    self._record(TransportEvent("ACK", sequence))

                disposition = self._ack_tracker.observe(header["sequence"])
                if header["flag"] == 1:
                    await self._send_ack_only()
                if disposition in {"DUPLICATE", "STALE"} or message.name == "AckOnly":
                    continue
                if message.name == "RudpBindAccepted" and self._bind_future is not None:
                    if not self._bind_future.done():
                        self._bind_future.set_result(message)
                    continue
                try:
                    self._messages.put_nowait(message)
                except asyncio.QueueFull as exc:
                    raise ProtocolError("RUDP application queue is full") from exc
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self._fail(exc)

    async def _maintenance_loop(self) -> None:
        try:
            while not self._closed:
                now = time.monotonic()
                for sequence in self._reliable.expire(now=now):
                    self._record(TransportEvent("TIMEOUT", sequence))
                await self._flush_reliable(now)
                if self._epoch != 0 and now - self._last_heartbeat >= self._heartbeat_interval:
                    sequence = self._take_sequence()
                    header = self._header(24, 4, sequence)
                    await self._loop.sock_sendall(
                        self._socket, encode_rudp_message("RudpHeartbeat", header, {})
                    )
                    self._record(TransportEvent("HEARTBEAT", sequence, 1))
                    self._last_heartbeat = now
                await asyncio.sleep(min(0.02, self._heartbeat_interval / 2))
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self._fail(exc)

    async def _flush_reliable(self, now: float) -> None:
        for transmission in self._reliable.poll(now=now):
            await self._loop.sock_sendall(self._socket, transmission.datagram)
            self._record(
                TransportEvent(
                    transmission.kind, transmission.sequence, transmission.attempt
                )
            )

    async def _send_ack_only(self) -> None:
        sequence = self._take_sequence()
        header = self._header(0, 2, sequence)
        await self._loop.sock_sendall(
            self._socket, encode_rudp_message("AckOnly", header, {})
        )
        self._record(TransportEvent("ACK_ONLY", sequence, 1))

    def _header(
        self,
        message_id: object,
        flag: object,
        sequence: int,
        *,
        epoch: int | None = None,
    ) -> dict[str, int]:
        if not isinstance(message_id, int) or not isinstance(flag, int):
            raise ProtocolError("unknown RUDP message")
        ack, ack_bits = self._ack_tracker.state
        return {
            "flag": flag,
            "sessionId": self._session_id,
            "sessionGeneration": self._generation,
            "transportEpoch": self._epoch if epoch is None else epoch,
            "sequence": sequence,
            "ack": ack,
            "ackBits": ack_bits,
            "messageId": message_id,
        }

    def _take_sequence(self) -> int:
        sequence = self._next_sequence
        self._next_sequence = 1 if sequence == 0xFFFFFFFF else sequence + 1
        return sequence

    def _record(self, event: TransportEvent) -> None:
        if len(self._transport_events) >= 4096:
            raise ProtocolError("RUDP transport event buffer is full")
        self._transport_events.append(event)

    def _fail(self, error: Exception) -> None:
        self._fatal_error = error
        if self._bind_future is not None and not self._bind_future.done():
            self._bind_future.set_exception(error)

    def _raise_if_failed(self) -> None:
        if self._closed:
            raise ProtocolError("RUDP peer is closed")
        if self._fatal_error is not None:
            raise ProtocolError("RUDP peer receive/maintenance task failed") from self._fatal_error


def _validate_epoch(name: str, value: object) -> None:
    epoch = _uint(value, "transportEpoch", 32)
    if name == "RudpBindHello" and epoch != 0:
        raise ProtocolError("RudpBindHello transportEpoch must be zero")
    if name not in {"RudpBindHello", "AckOnly"} and epoch == 0:
        raise ProtocolError(f"{name} transportEpoch must be nonzero")


def _identifier(value: object, field: str) -> tuple[int, int]:
    if not isinstance(value, dict):
        raise ProtocolError(f"{field} must be an object")
    high = _uint(value.get("high"), f"{field}.high", 64)
    low = _uint(value.get("low"), f"{field}.low", 64)
    if high == 0 and low == 0:
        raise ProtocolError(f"{field} must be nonzero")
    return high, low


def _hit_points(value: object) -> int:
    hit_points = _uint(value, "hitPoints", 32)
    if hit_points > 1600 or hit_points % 20:
        raise ProtocolError("hit points violate ruleset v1")
    return hit_points


def _bounded_position(value: object, field: str) -> int:
    position = _signed(value, field, 32)
    if not -10000 <= position <= 10000:
        raise ProtocolError(f"{field} is outside arena bounds")
    return position


def _signed(value: object, field: str, bits: int, *, forbidden: int | None = None) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not -(1 << (bits - 1)) <= value < 1 << (bits - 1)
        or value == forbidden
    ):
        raise ProtocolError(f"{field} must be valid int{bits}")
    return value


def _positive(value: object, field: str, bits: int) -> int:
    number = _uint(value, field, bits)
    if number == 0:
        raise ProtocolError(f"{field} must be nonzero")
    return number


def _uint(value: object, field: str, bits: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 1 << bits:
        raise ProtocolError(f"{field} must be uint{bits}")
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


def _unpack(format_string: str, payload: bytes) -> tuple[Any, ...]:
    expected = struct.calcsize(format_string)
    if len(payload) != expected:
        raise ProtocolError(f"RUDP payload must contain {expected} bytes")
    return struct.unpack(format_string, payload)


def _is_newer(candidate: int, reference: int) -> bool:
    return candidate != reference and ((candidate - reference) & 0xFFFFFFFF) < 0x80000000
