"""Canonical 10-player Loot Race workload.

The driver schedules intent only.  Server projections and terminal results are
the sole source of gameplay state and progress.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from enum import Enum, auto
from typing import Any

from ..protocol.rudp import RudpMessage
from ..protocol.tcp import TcpMessage


PLAYER_COUNT = 10
MOVE_INTERVAL_MS = 100
DIRECTION_INTERVAL_MS = 200
ATTACK_INTERVAL_MS = 750
APPLICATION_RETRY_MS = 200
MAX_APPLICATION_ATTEMPTS = 5
ATTACK_RANGE_MILLIMETERS = 3000
CLAIM_RANGE_MILLIMETERS = 1500
_DIRECTIONS = (
    (32767, 0),
    (23170, 23170),
    (0, 32767),
    (-23170, 23170),
    (-32767, 0),
    (-23170, -23170),
    (0, -32767),
    (23170, -23170),
)


class WorkloadViolation(ValueError):
    """A server observation violates the fixed workload contract."""


class PlayerPhase(Enum):
    NEW = auto()
    AUTHENTICATED = auto()
    IN_ROOM = auto()
    READY = auto()
    LOADING = auto()
    GAMEPLAY = auto()
    LOOT = auto()
    RESULT = auto()
    REOPENED = auto()


@dataclass(frozen=True)
class ScheduledCommand:
    at_ms: int
    participant_index: int
    channel: str
    name: str
    fields: dict[str, Any]
    application_attempt: int = 1


@dataclass
class _PendingIntent:
    name: str
    fields: dict[str, Any]
    next_retry_ms: int
    attempt: int = 1


class _Player:
    def __init__(self, index: int, seed: int) -> None:
        self.index = index
        self.seed = seed
        self.phase = PlayerPhase.NEW
        self.session_id: int | None = None
        self.session_generation: int | None = None
        self.room_id: int | None = None
        self.battle_instance_id: int | None = None
        self.rudp_bound = False
        self.gameplay_started_ms: int | None = None
        self.next_move_ms: int | None = None
        self.next_attack_ms: int | None = None
        self.action_sequence = 0
        self.monster_id: int | None = None
        self.monster_position: tuple[int, int] | None = None
        self.player_position: tuple[int, int] | None = None
        self.available_drop_ids: tuple[int, ...] = ()
        self.drop_positions: dict[int, tuple[int, int]] = {}
        self.next_loot_ms: int | None = None
        self.pending: _PendingIntent | None = None
        self.intent_ordinal = 0
        self.attack_intent_count = 0
        self.attack_result_count = 0
        self.combat_outcome: str | None = None
        self.claim_intent_count = 0
        self.claim_result_count = 0
        self.claim_succeeded = False
        self.final_outcome: str | None = None

    def welcome(self, fields: dict[str, Any]) -> None:
        identity = (fields.get("sessionId"), fields.get("sessionGeneration"))
        if not all(isinstance(value, int) and value > 0 for value in identity):
            raise WorkloadViolation("Welcome identity must be positive")
        if self.session_id is not None and identity != (
            self.session_id,
            self.session_generation,
        ):
            raise WorkloadViolation("player identity changed")
        self.session_id, self.session_generation = identity
        self.phase = PlayerPhase.AUTHENTICATED

    def room_detail(self, fields: dict[str, Any]) -> None:
        room_id = _positive(fields.get("roomId"), "roomId")
        if self.room_id is not None and room_id != self.room_id:
            raise WorkloadViolation("same-Room cycle changed RoomId")
        members = fields.get("members")
        if not isinstance(members, list):
            raise WorkloadViolation("Room detail members are absent")
        own = next(
            (
                member
                for member in members
                if member.get("sessionId") == self.session_id
                and member.get("sessionGeneration") == self.session_generation
            ),
            None,
        )
        if own is None:
            return
        self.room_id = room_id
        if self.phase in {
            PlayerPhase.AUTHENTICATED,
            PlayerPhase.IN_ROOM,
            PlayerPhase.READY,
            PlayerPhase.REOPENED,
        }:
            self.phase = PlayerPhase.READY if own.get("ready") is True else PlayerPhase.IN_ROOM

    def load_entry(self, fields: dict[str, Any]) -> int:
        room_id = _positive(fields.get("roomId"), "roomId")
        battle_id = _positive(fields.get("battleInstanceId"), "battleInstanceId")
        if self.room_id != room_id:
            raise WorkloadViolation("ArenaLoadEntry changed RoomId")
        if self.battle_instance_id is not None and battle_id <= self.battle_instance_id:
            raise WorkloadViolation("BattleInstanceId did not increase")
        self.battle_instance_id = battle_id
        self.phase = PlayerPhase.LOADING
        self.monster_id = None
        self.monster_position = None
        self.player_position = None
        self.available_drop_ids = ()
        self.drop_positions = {}
        self.next_loot_ms = None
        self.pending = None
        self.attack_intent_count = 0
        self.attack_result_count = 0
        self.combat_outcome = None
        self.claim_intent_count = 0
        self.claim_result_count = 0
        self.claim_succeeded = False
        self.final_outcome = None
        return battle_id

    def bind(self, header: dict[str, int]) -> None:
        if self.session_id is not None and header.get("sessionId") != self.session_id:
            raise WorkloadViolation("RUDP bind identity differs from TCP identity")
        self.rudp_bound = True

    def gameplay_start(self, fields: dict[str, Any], at_ms: int) -> None:
        if fields.get("roomId") != self.room_id or fields.get("battleInstanceId") != self.battle_instance_id:
            raise WorkloadViolation("gameplay start identity differs from load entry")
        participants = fields.get("participants")
        if not isinstance(participants, list) or not any(
            participant.get("sessionId") == self.session_id
            and participant.get("sessionGeneration") == self.session_generation
            for participant in participants
        ):
            raise WorkloadViolation("player is absent from gameplay participants")
        self.phase = PlayerPhase.GAMEPLAY
        self.gameplay_started_ms = at_ms
        self.next_move_ms = at_ms
        self.next_attack_ms = at_ms + ATTACK_INTERVAL_MS
        self.action_sequence = 0
        self.pending = None

    def monster_spawned(self, fields: dict[str, Any]) -> None:
        self._same_battle(fields)
        self.monster_id = _positive(fields.get("monsterId"), "monsterId")
        self.monster_position = _position(fields)

    def state_snapshot(self, fields: dict[str, Any]) -> None:
        self._same_battle(fields)
        players = fields.get("players")
        own = next(
            (
                player
                for player in players
                if player.get("sessionId") == self.session_id
            ),
            None,
        ) if isinstance(players, list) else None
        self.player_position = None if own is None else _position(own)

    def drop_spawned(self, fields: dict[str, Any], at_ms: int) -> None:
        self._same_battle(fields)
        drop_id = _positive(fields.get("dropId"), "dropId")
        self.drop_positions[drop_id] = _position(fields)
        self.available_drop_ids = tuple(sorted({*self.available_drop_ids, drop_id}))
        if self.next_loot_ms is None:
            self.next_loot_ms = at_ms + self.index * 100

    def completed_cycle(self, battle_id: int) -> bool:
        return (
            self.battle_instance_id == battle_id
            and self.attack_intent_count > 0
            and self.attack_result_count > 0
            and self.combat_outcome == "MONSTER_DEFEATED"
            and self.claim_intent_count > 0
            and self.claim_result_count > 0
            and self.final_outcome == "MONSTER_DEFEATED"
            and self.pending is None
        )

    def combat_terminal(self, fields: dict[str, Any]) -> None:
        self._same_battle(fields)
        outcome = fields.get("combatOutcome")
        if outcome not in {"MONSTER_DEFEATED", "COMBAT_TIMEOUT"}:
            raise WorkloadViolation("combat terminal outcome is not terminal")
        self.combat_outcome = outcome
        if self.phase not in {PlayerPhase.RESULT, PlayerPhase.REOPENED}:
            self.phase = PlayerPhase.LOOT

    def drops(self, fields: dict[str, Any], at_ms: int) -> None:
        self._same_battle(fields)
        drops = fields.get("drops")
        if not isinstance(drops, list):
            raise WorkloadViolation("DropStateSnapshot drops are absent")
        available: list[int] = []
        for drop in drops:
            drop_id = _positive(drop.get("dropId"), "dropId")
            self.drop_positions[drop_id] = _position(drop)
            if drop.get("state") == "AVAILABLE":
                available.append(drop_id)
        available = sorted(available)
        self.available_drop_ids = tuple(available)
        if not available:
            self.next_loot_ms = None
        elif self.next_loot_ms is None:
            self.next_loot_ms = at_ms + self.index * 100

    def terminal_result(self, name: str, fields: dict[str, Any], at_ms: int) -> None:
        self._same_battle(fields)
        intent_name = name.replace("TerminalResult", "Intent")
        if self.pending is None or self.pending.name != intent_name:
            return
        if fields.get("commandId") != self.pending.fields.get("commandId"):
            return
        if name == "AttackTerminalResult":
            self.attack_result_count += 1
        else:
            drop_id = self.pending.fields["dropId"]
            if fields.get("dropId") != drop_id:
                raise WorkloadViolation("loot result changed DropId")
            self.claim_result_count += 1
            self.claim_succeeded = self.claim_succeeded or fields.get("resultCode") == "OK"
            if fields.get("resultCode") in {
                "OK",
                "UNKNOWN_DROP",
                "ALREADY_CLAIMED",
                "RESOLUTION_CLOSED",
            }:
                self.available_drop_ids = tuple(
                    available
                    for available in self.available_drop_ids
                    if available != drop_id
                )
        self.pending = None
        if (
            name == "ClaimLootTerminalResult"
            and self.available_drop_ids
            and self.phase == PlayerPhase.LOOT
        ):
            self.next_loot_ms = at_ms + APPLICATION_RETRY_MS

    def final_result(self, fields: dict[str, Any]) -> None:
        self._same_battle(fields)
        if fields.get("roomId") != self.room_id:
            raise WorkloadViolation("FinalResult changed RoomId")
        outcome = fields.get("outcome")
        if outcome not in {"MONSTER_DEFEATED", "COMBAT_TIMEOUT"}:
            raise WorkloadViolation("FinalResult outcome is not terminal")
        self.final_outcome = outcome
        self.phase = PlayerPhase.RESULT

    def tick(self, at_ms: int) -> list[ScheduledCommand]:
        commands: list[ScheduledCommand] = []
        if self.phase in {PlayerPhase.GAMEPLAY, PlayerPhase.LOOT}:
            while self.next_move_ms is not None and self.next_move_ms <= at_ms:
                commands.append(self._move(self.next_move_ms))
                self.next_move_ms += MOVE_INTERVAL_MS
        if self.phase == PlayerPhase.GAMEPLAY:
            if self.pending is not None:
                retry = self._retry(at_ms)
                if retry is not None:
                    commands.append(retry)
            elif (
                self.monster_id is not None
                and self.monster_position is not None
                and self.player_position is not None
                and _within_range(
                    self.player_position,
                    self.monster_position,
                    ATTACK_RANGE_MILLIMETERS,
                )
                and self.next_attack_ms is not None
                and self.next_attack_ms <= at_ms
            ):
                commands.append(self._new_attack(max(self.next_attack_ms, at_ms)))
        elif self.phase == PlayerPhase.LOOT and not self.claim_succeeded:
            if self.pending is not None:
                retry = self._retry(at_ms)
                if retry is not None:
                    commands.append(retry)
            else:
                target = self._target_drop_id()
                if (
                    target is not None
                    and self.player_position is not None
                    and _within_range(
                        self.player_position,
                        self.drop_positions[target],
                        CLAIM_RANGE_MILLIMETERS,
                    )
                    and self.next_loot_ms is not None
                    and self.next_loot_ms <= at_ms
                ):
                    commands.append(self._new_claim(at_ms))
        return commands

    def _move(self, at_ms: int) -> ScheduledCommand:
        assert self.gameplay_started_ms is not None and self.battle_instance_id is not None
        target = self._target_drop_id() if self.phase == PlayerPhase.LOOT else None
        if target is not None and self.player_position is not None:
            desired_x, desired_y = _direction_toward(
                self.player_position, self.drop_positions[target]
            )
        else:
            slot = (at_ms - self.gameplay_started_ms) // DIRECTION_INTERVAL_MS
            offset = _digest_int(self.seed, self.index, "direction") % len(_DIRECTIONS)
            desired_x, desired_y = _DIRECTIONS[(offset + slot) % len(_DIRECTIONS)]
        self.action_sequence += 1
        return ScheduledCommand(
            at_ms,
            self.index,
            "RUDP",
            "MoveIntent",
            {
                "battleInstanceId": self.battle_instance_id,
                "actionSequence": self.action_sequence,
                "desiredX": desired_x,
                "desiredY": desired_y,
                "inputFlags": 0,
            },
        )

    def _new_attack(self, at_ms: int) -> ScheduledCommand:
        assert self.battle_instance_id is not None and self.monster_id is not None
        fields = {
            "commandId": self._command_id("attack"),
            "battleInstanceId": self.battle_instance_id,
            "targetHint": self.monster_id,
        }
        self.pending = _PendingIntent("AttackIntent", fields, at_ms + APPLICATION_RETRY_MS)
        self.next_attack_ms = at_ms + ATTACK_INTERVAL_MS
        self.attack_intent_count += 1
        return ScheduledCommand(at_ms, self.index, "RUDP", "AttackIntent", fields)

    def _new_claim(self, at_ms: int) -> ScheduledCommand:
        assert self.battle_instance_id is not None
        target = self._target_drop_id()
        assert target is not None
        fields = {
            "commandId": self._command_id("claim"),
            "battleInstanceId": self.battle_instance_id,
            "dropId": target,
        }
        self.pending = _PendingIntent("ClaimLootIntent", fields, at_ms + APPLICATION_RETRY_MS)
        self.next_loot_ms = None
        self.claim_intent_count += 1
        return ScheduledCommand(at_ms, self.index, "RUDP", "ClaimLootIntent", fields)

    def _target_drop_id(self) -> int | None:
        if self.pending is not None and self.pending.name == "ClaimLootIntent":
            pending_drop = self.pending.fields["dropId"]
            if pending_drop in self.drop_positions:
                return pending_drop
        if not self.available_drop_ids or self.battle_instance_id is None:
            return None
        known = sorted(self.drop_positions)
        offset = (
            _digest_int(self.seed, self.battle_instance_id, "loot-order") + self.index
        ) % len(known)
        available = set(self.available_drop_ids)
        return next(
            drop_id
            for drop_id in known[offset:] + known[:offset]
            if drop_id in available
        )

    def _retry(self, at_ms: int) -> ScheduledCommand | None:
        assert self.pending is not None
        if self.pending.attempt >= MAX_APPLICATION_ATTEMPTS or self.pending.next_retry_ms > at_ms:
            return None
        self.pending.attempt += 1
        scheduled_at = self.pending.next_retry_ms
        self.pending.next_retry_ms += APPLICATION_RETRY_MS
        return ScheduledCommand(
            scheduled_at,
            self.index,
            "RUDP",
            self.pending.name,
            self.pending.fields,
            self.pending.attempt,
        )

    def _command_id(self, purpose: str) -> dict[str, int]:
        self.intent_ordinal += 1
        digest = hashlib.sha256(
            f"{self.seed}:{self.index}:{self.battle_instance_id}:{purpose}:{self.intent_ordinal}".encode()
        ).digest()
        high = int.from_bytes(digest[:8], "big") or 1
        low = int.from_bytes(digest[8:16], "big") or 1
        return {"high": high, "low": low}

    def _same_battle(self, fields: dict[str, Any]) -> None:
        if fields.get("battleInstanceId") != self.battle_instance_id:
            raise WorkloadViolation("server message belongs to another BattleInstance")


class LootRaceCoordinator:
    """Coordinates one deterministic 10-player Room for a fixed cycle count."""

    def __init__(self, seed: int, required_cycles: int = 2) -> None:
        if isinstance(seed, bool) or not isinstance(seed, int) or seed < 0:
            raise ValueError("seed must be a non-negative integer")
        if (
            isinstance(required_cycles, bool)
            or not isinstance(required_cycles, int)
            or required_cycles <= 0
        ):
            raise ValueError("required_cycles must be a positive integer")
        self.seed = seed
        self.required_cycles = required_cycles
        self.players = tuple(_Player(index, seed) for index in range(PLAYER_COUNT))
        self.room_id: int | None = None
        self.active_battle_id: int | None = None
        self.completed_battle_ids: tuple[int, ...] = ()
        self._commands: list[ScheduledCommand] = []
        self._schedule_log: list[ScheduledCommand] = []
        self._created = False
        self._joined = False
        self._ready_scheduled = False
        self._host_start_scheduled = False
        self._load_entries: set[int] = set()
        self._load_complete_battle: int | None = None
        self._final_results: set[int] = set()
        self._reopened_battle: int | None = None
        self._drain_requested = False
        self._drained = False
        self._request_sequence = 0

    @property
    def complete(self) -> bool:
        return len(self.completed_battle_ids) == self.required_cycles

    @property
    def schedule_log(self) -> tuple[ScheduledCommand, ...]:
        return tuple(self._schedule_log)

    @property
    def drained(self) -> bool:
        return self._drained

    def request_drain(self) -> None:
        self._drain_requested = True
        if self.active_battle_id in self.completed_battle_ids:
            self._drained = True

    def take_commands(self) -> list[ScheduledCommand]:
        commands, self._commands = self._commands, []
        return commands

    def on_tcp(self, participant_index: int, message: TcpMessage, at_ms: int) -> None:
        player = self._player(participant_index)
        if message.name == "Welcome":
            player.welcome(message.fields)
            if not self._created and all(p.phase == PlayerPhase.AUTHENTICATED for p in self.players):
                self._created = True
                self._emit(at_ms, 0, "TCP", "CreateRoom", title="load-room", capacity=10)
        elif message.name == "RoomDetailProjection":
            player.room_detail(message.fields)
            self._room_projection(message.fields, at_ms)
        elif message.name == "ArenaLoadEntry":
            battle_id = player.load_entry(message.fields)
            self._load_entry(participant_index, battle_id, at_ms)
        elif message.name == "ArenaGameplayStart":
            player.gameplay_start(message.fields, at_ms)
        elif message.name == "FinalResult":
            player.final_result(message.fields)
            self._final_results.add(participant_index)
            self._maybe_complete_cycle(at_ms)

    def on_rudp(self, participant_index: int, message: RudpMessage, at_ms: int) -> None:
        player = self._player(participant_index)
        if message.name == "RudpBindAccepted":
            player.bind(message.header)
            self._maybe_finish_load(at_ms)
        elif message.name == "MonsterSpawned":
            player.monster_spawned(message.fields)
        elif message.name == "StateSnapshot":
            player.state_snapshot(message.fields)
        elif message.name == "CombatTerminalEvent":
            player.combat_terminal(message.fields)
        elif message.name == "DropSpawned":
            player.drop_spawned(message.fields, at_ms)
        elif message.name == "DropStateSnapshot":
            player.drops(message.fields, at_ms)
        elif message.name in {"AttackTerminalResult", "ClaimLootTerminalResult"}:
            player.terminal_result(message.name, message.fields, at_ms)
        self._maybe_complete_cycle(at_ms)

    def tick(self, at_ms: int) -> None:
        for player in self.players:
            for command in player.tick(at_ms):
                self._record(command)

    def _room_projection(self, fields: dict[str, Any], at_ms: int) -> None:
        room_id = _positive(fields.get("roomId"), "roomId")
        if self.room_id is not None and room_id != self.room_id:
            raise WorkloadViolation("Room projection changed RoomId")
        self.room_id = room_id
        members = fields.get("members")
        if not isinstance(members, list):
            raise WorkloadViolation("Room projection members are absent")
        if len(members) == 1 and not self._joined:
            self._joined = True
            for index in range(1, PLAYER_COUNT):
                self._emit(at_ms, index, "TCP", "JoinRoom", roomId=room_id)
            return
        if len(members) != PLAYER_COUNT:
            return
        all_ready = all(member.get("ready") is True for member in members)
        all_unready = all(member.get("ready") is False for member in members)
        if all_unready and self.active_battle_id is not None:
            self._reopened_battle = self.active_battle_id
            for player in self.players:
                player.phase = PlayerPhase.REOPENED
            self._maybe_complete_cycle(at_ms)
            return
        if all_unready and not self._ready_scheduled:
            self._schedule_ready(at_ms)
        elif all_ready and not self._host_start_scheduled:
            self._host_start_scheduled = True
            self._emit(at_ms, 0, "TCP", "HostStartRequest")

    def _schedule_ready(self, at_ms: int) -> None:
        self._ready_scheduled = True
        for index in range(PLAYER_COUNT):
            self._emit(at_ms, index, "TCP", "SetReady", ready=True)

    def _maybe_complete_cycle(self, at_ms: int) -> None:
        battle_id = self.active_battle_id
        if (
            battle_id is None
            or self._reopened_battle != battle_id
            or len(self._final_results) != PLAYER_COUNT
            or battle_id in self.completed_battle_ids
            or not all(player.completed_cycle(battle_id) for player in self.players)
        ):
            return
        self.completed_battle_ids += (battle_id,)
        self._final_results.clear()
        self._ready_scheduled = False
        self._host_start_scheduled = False
        if self._drain_requested or self.complete:
            self._drained = True
        else:
            self._schedule_ready(at_ms)

    def _load_entry(self, participant_index: int, battle_id: int, at_ms: int) -> None:
        if self.completed_battle_ids and battle_id <= self.completed_battle_ids[-1]:
            raise WorkloadViolation("BattleInstanceId did not increase after reopen")
        if self.active_battle_id != battle_id:
            if (
                self._load_entries
                and self.active_battle_id is not None
                and self.active_battle_id not in self.completed_battle_ids
            ):
                raise WorkloadViolation("participants entered different BattleInstances")
            self.active_battle_id = battle_id
            self._load_entries.clear()
            self._load_complete_battle = None
            self._reopened_battle = None
        self._load_entries.add(participant_index)
        self._maybe_finish_load(at_ms)

    def _maybe_finish_load(self, at_ms: int) -> None:
        if (
            self.active_battle_id is None
            or self._load_complete_battle == self.active_battle_id
            or len(self._load_entries) != PLAYER_COUNT
            or not all(player.rudp_bound for player in self.players)
        ):
            return
        self._load_complete_battle = self.active_battle_id
        for index in range(PLAYER_COUNT):
            self._emit(
                at_ms,
                index,
                "TCP",
                "ArenaLoadComplete",
                roomId=self.room_id,
                battleInstanceId=self.active_battle_id,
            )

    def _emit(self, at_ms: int, participant_index: int, channel: str, name: str, **fields: Any) -> None:
        if channel == "TCP":
            self._request_sequence += 1
            fields = {"requestId": self._request_sequence, **fields}
        self._record(ScheduledCommand(at_ms, participant_index, channel, name, fields))

    def _record(self, command: ScheduledCommand) -> None:
        self._commands.append(command)
        self._schedule_log.append(command)

    def _player(self, participant_index: int) -> _Player:
        if isinstance(participant_index, bool) or not 0 <= participant_index < PLAYER_COUNT:
            raise WorkloadViolation("participant index is outside the fixed Room")
        return self.players[participant_index]


def _positive(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise WorkloadViolation(f"{field} must be a positive integer")
    return value


def _position(fields: dict[str, Any]) -> tuple[int, int]:
    values = (fields.get("posXMillimeter"), fields.get("posYMillimeter"))
    if any(isinstance(value, bool) or not isinstance(value, int) for value in values):
        raise WorkloadViolation("server position must contain integer coordinates")
    return values


def _within_range(
    origin: tuple[int, int], target: tuple[int, int], maximum: int
) -> bool:
    delta_x = target[0] - origin[0]
    delta_y = target[1] - origin[1]
    return delta_x * delta_x + delta_y * delta_y <= maximum * maximum


def _direction_toward(
    origin: tuple[int, int], target: tuple[int, int]
) -> tuple[int, int]:
    delta_x = target[0] - origin[0]
    delta_y = target[1] - origin[1]
    if delta_x and delta_y:
        return (23170 if delta_x > 0 else -23170, 23170 if delta_y > 0 else -23170)
    if delta_x:
        return (32767 if delta_x > 0 else -32767, 0)
    if delta_y:
        return (0, 32767 if delta_y > 0 else -32767)
    return (0, 0)


def _digest_int(*parts: object) -> int:
    return int.from_bytes(hashlib.sha256(":".join(map(str, parts)).encode()).digest()[:8], "big")
