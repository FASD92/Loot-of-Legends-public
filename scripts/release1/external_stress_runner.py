#!/usr/bin/env python3
"""Run a minimal external Release 1 TCP room-fill workload."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time
import zlib
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "release1.external_stress_runner.v1"
WORKLOAD = "tcp_room_fill"
MAX_TCP_PACKET_SIZE = 1024
MAX_RUDP_PACKET_SIZE = 1200
RUDP_HEADER_SIZE = 28
FIRST_CLIENT_ID = 100000
RUDP_CLIENT_VERSION = 1

TYPE_WELCOME = 0x0001
TYPE_AUTHENTICATE_GAME_SESSION = 0x0003
TYPE_CREATE_ROOM_REQUEST = 0x0101
TYPE_CREATE_ROOM_RESPONSE = 0x0102
TYPE_JOIN_ROOM_REQUEST = 0x0103
TYPE_JOIN_ROOM_RESPONSE = 0x0104
TYPE_READY_ROOM_REQUEST = 0x0108
TYPE_READY_ROOM_RESPONSE = 0x0109
TYPE_BATTLE_START = 0x010A
TYPE_MONSTER_SPAWN = 0x010B
TYPE_MONSTER_DEATH = 0x010D
TYPE_BATTLE_START_ROSTER = 0x0118
TYPE_MONSTER_HEALTH_SNAPSHOT = 0x0119
TYPE_DROP_LIST_SNAPSHOT_V2 = 0x011A
TYPE_ARENA_LOAD_COMPLETE = 0x011B
TYPE_LOBBY_RETURN_VISIBILITY = 0x011C
TYPE_HOST_START_BATTLE_REQUEST = 0x0120
TYPE_HOST_START_BATTLE_RESPONSE = 0x0121
TYPE_ARENA_GAMEPLAY_START = 0x0124
TYPE_BATTLE_FINAL_RANKING = 0x0125
TYPE_BATTLE_LOAD_ENTRY = 0x0126
TYPE_ERROR = 0x01FF
TYPE_ROOM_LIST_SNAPSHOT = 0x0107
TYPE_LOOT_RESOLVED = 0x0110
TYPE_INVENTORY_SNAPSHOT = 0x0112
TCP_PACKET_TYPE_NAMES = {
    TYPE_WELCOME: "welcome",
    TYPE_AUTHENTICATE_GAME_SESSION: "authenticate_game_session",
    TYPE_CREATE_ROOM_REQUEST: "create_room_request",
    TYPE_CREATE_ROOM_RESPONSE: "create_room_response",
    TYPE_JOIN_ROOM_REQUEST: "join_room_request",
    TYPE_JOIN_ROOM_RESPONSE: "join_room_response",
    TYPE_ROOM_LIST_SNAPSHOT: "room_list_snapshot",
    TYPE_READY_ROOM_REQUEST: "ready_room_request",
    TYPE_READY_ROOM_RESPONSE: "ready_room_response",
    TYPE_BATTLE_START: "battle_start",
    TYPE_MONSTER_SPAWN: "monster_spawn",
    TYPE_MONSTER_DEATH: "monster_death",
    TYPE_LOOT_RESOLVED: "loot_resolved",
    TYPE_INVENTORY_SNAPSHOT: "inventory_snapshot",
    TYPE_BATTLE_START_ROSTER: "battle_start_roster",
    TYPE_MONSTER_HEALTH_SNAPSHOT: "monster_health_snapshot",
    TYPE_DROP_LIST_SNAPSHOT_V2: "drop_list_snapshot_v2",
    TYPE_ARENA_LOAD_COMPLETE: "arena_load_complete",
    TYPE_LOBBY_RETURN_VISIBILITY: "lobby_return_visibility",
    TYPE_HOST_START_BATTLE_REQUEST: "host_start_battle_request",
    TYPE_HOST_START_BATTLE_RESPONSE: "host_start_battle_response",
    TYPE_ARENA_GAMEPLAY_START: "arena_gameplay_start",
    TYPE_BATTLE_FINAL_RANKING: "battle_final_ranking",
    TYPE_BATTLE_LOAD_ENTRY: "battle_load_entry",
    TYPE_ERROR: "error",
}

RUDP_CHANNEL_CONTROL = 0x01
RUDP_CHANNEL_INPUT = 0x02
RUDP_CHANNEL_EVENT = 0x04
RUDP_FLAG_RELIABLE = 0x01
RUDP_FLAG_ACK_ONLY = 0x02
RUDP_TYPE_HELLO = 0x1001
RUDP_TYPE_INPUT_COMMAND = 0x1002
RUDP_TYPE_BATTLE_START = 0x1003
RUDP_TYPE_STATE_SNAPSHOT = 0x1004
RUDP_TYPE_GAME_EVENT = 0x1005
RUDP_TYPE_META_RESPONSE = 0x1006
RUDP_TYPE_BATTLE_START_ROSTER = 0x1007
RUDP_TYPE_ERROR = 0x2001
RUDP_INPUT_READY = 0x01
RUDP_INPUT_CLICK_LOOT = 0x03
RUDP_INPUT_MOVE = 0x04
RUDP_INPUT_ATTACK = 0x05
RUDP_GAME_EVENT_LOOT_RESOLVED = 0x0002
RUDP_LOOT_RESOLVED_BODY_SIZE = 22
RUDP_LOOT_RESOLVED_PAYLOAD_SIZE = 26
LOOT_SMOKE_ATTACK_ROUNDS = 4


def run_external_tcp_room_fill(
    host: str,
    tcp_port: int,
    participants: int,
    room_size: int = 10,
    timeout_sec: float = 5.0,
    udp_port: int | None = None,
    auth_token_prefix: str | None = None,
    start_battle: bool = False,
    loot_smoke: bool = False,
    move_duration_sec: float = 0.0,
    move_rate_hz: float = 0.0,
    attack_duration_sec: float = 0.0,
    attack_rate_hz: float = 0.0,
    battle_cycles: int = 1,
    client_id_base: int = FIRST_CLIENT_ID,
    rudp_handshake_settle_sec: float = 0.0,
) -> dict[str, Any]:
    _validate_config(
        host,
        tcp_port,
        participants,
        room_size,
        timeout_sec,
        udp_port,
        auth_token_prefix,
        start_battle,
        loot_smoke,
        move_duration_sec,
        move_rate_hz,
        attack_duration_sec,
        attack_rate_hz,
        battle_cycles,
        client_id_base,
        rudp_handshake_settle_sec,
    )
    result = _base_result(
        host,
        tcp_port,
        participants,
        room_size,
        udp_port,
        auth_token_prefix,
        start_battle,
        loot_smoke,
        move_duration_sec,
        move_rate_hz,
        attack_duration_sec,
        attack_rate_hz,
        battle_cycles,
        client_id_base,
        rudp_handshake_settle_sec,
    )
    clients: list[dict[str, Any]] = []
    move_iterations = _move_iterations(move_duration_sec, move_rate_hz)
    attack_rounds = _attack_rounds(attack_duration_sec, attack_rate_hz, loot_smoke)
    failure_stage = "connect"
    failure_cycle: int | None = None
    failure_room_first: int | None = None
    failure_join_offset: int | None = None
    failure_client_index: int | None = None
    failure_client_id: int | None = None
    failure_session_id: int | None = None

    def track_failure_client(index: int) -> None:
        nonlocal failure_client_index, failure_client_id, failure_session_id
        failure_client_index = index
        failure_client_id = client_id_base + index
        failure_session_id = None
        if index < len(clients):
            failure_session_id = clients[index].get("session_id")

    try:
        for index in range(participants):
            failure_stage = "connect"
            track_failure_client(index)
            tcp_socket = socket.create_connection((host, tcp_port), timeout=timeout_sec)
            tcp_socket.settimeout(timeout_sec)
            client: dict[str, Any] = {
                "tcp": tcp_socket,
                "client_id": client_id_base + index,
            }
            clients.append(client)
            result["connected"] += 1
            if auth_token_prefix is not None:
                failure_stage = "authenticate"
                track_failure_client(index)
                tcp_socket.sendall(_authenticate_packet(_auth_token(auth_token_prefix, index)))
                result["auth_sent"] += 1
            failure_stage = "receive_welcome"
            track_failure_client(index)
            client["session_id"] = _receive_welcome(tcp_socket)
            result["welcome_received"] += 1
            if udp_port is not None:
                udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                udp_socket.settimeout(timeout_sec)
                client["udp"] = udp_socket

        for cycle_index in range(battle_cycles):
            failure_cycle = cycle_index
            ready_cmd_seq = _first_ready_cmd_seq(
                cycle_index,
                move_iterations,
                attack_rounds,
                loot_smoke,
                room_size,
            )
            for first in range(0, participants, room_size):
                failure_room_first = first
                failure_join_offset = None
                failure_stage = "create_room"
                track_failure_client(first)
                room_id = _create_room(
                    clients[first]["tcp"],
                    room_size,
                    timeout_sec,
                    result,
                )
                result["rooms_created"] += 1

                room_filled = True
                for offset in range(1, room_size):
                    failure_stage = "join_room"
                    failure_join_offset = offset
                    track_failure_client(first + offset)
                    player_count = _join_room(
                        clients[first + offset]["tcp"],
                        room_id,
                        timeout_sec,
                        result,
                    )
                    room_filled = room_filled and player_count == offset + 1
                if room_filled:
                    result["rooms_filled"] += 1

                room_clients = clients[first:first + room_size]
                if udp_port is not None:
                    failure_stage = "rudp_hello"
                    failure_join_offset = None
                    track_failure_client(first)
                    _send_rudp_hellos(host, udp_port, room_clients, result)
                    if rudp_handshake_settle_sec > 0:
                        time.sleep(rudp_handshake_settle_sec)

                if start_battle:
                    failure_stage = "tcp_start_battle"
                    track_failure_client(first)
                    spawned_monster = _send_tcp_start_battle(
                        room_clients,
                        result,
                        timeout_sec,
                    )
                else:
                    spawned_monster = None

                if udp_port is not None:
                    failure_stage = "rudp_ready"
                    track_failure_client(first)
                    _send_rudp_ready_and_ack_battle_start(
                        host,
                        udp_port,
                        room_id,
                        room_clients,
                        timeout_sec,
                        result,
                        ready_cmd_seq,
                    )
                    first_attack_cmd_seq = ready_cmd_seq + 1
                    if start_battle:
                        failure_stage = "rudp_move"
                        track_failure_client(first)
                        first_attack_cmd_seq = _send_rudp_move_workload(
                            host,
                            udp_port,
                            room_clients,
                            result,
                            move_iterations,
                            move_rate_hz,
                            ready_cmd_seq + 1,
                        )
                    if start_battle and spawned_monster is not None and loot_smoke:
                        failure_stage = "rudp_attack_loot"
                        track_failure_client(first)
                        _send_rudp_attack_until_drop_and_click_loot(
                            host,
                            udp_port,
                            room_clients,
                            spawned_monster,
                            timeout_sec,
                            result,
                            first_attack_cmd_seq,
                            attack_rounds,
                        )
                        if battle_cycles > 1:
                            failure_stage = "battle_result_lobby_return"
                            track_failure_client(first)
                            _recv_battle_result_and_lobby_return_fanout(
                                room_clients,
                                room_id,
                                timeout_sec,
                                result,
                            )
                    elif start_battle and spawned_monster is not None:
                        failure_stage = "rudp_attack_health"
                        track_failure_client(first)
                        _send_rudp_attack_and_recv_health_snapshot(
                            host,
                            udp_port,
                            room_clients,
                            spawned_monster,
                            timeout_sec,
                            result,
                            first_attack_cmd_seq,
                        )

        expected_room_count = (participants // room_size) * battle_cycles
        expected_participant_cycles = participants * battle_cycles
        room_fill_valid = result["rooms_filled"] == expected_room_count
        tcp_battle_received = (
            result["tcp_battle_start_received"] +
            result["tcp_battle_start_roster_received"]
        )
        tcp_battle_valid = not start_battle or tcp_battle_received == expected_participant_cycles
        arena_entry_valid = not start_battle or (
            result["tcp_battle_load_entry_received"] == expected_participant_cycles and
            result["tcp_arena_load_complete_sent"] == expected_participant_cycles and
            result["tcp_arena_gameplay_start_received"] == expected_participant_cycles
        )
        monster_spawn_valid = (
            not start_battle or result["tcp_monster_spawn_received"] == expected_participant_cycles
        )
        health_snapshot_valid = (
            not start_battle or
            udp_port is None or
            loot_smoke or
            result["tcp_monster_health_snapshot_received"] == expected_participant_cycles
        )
        loot_fanout_target = result["rudp_click_loot_target_sent"]
        loot_resolved_fanout_target = len(result["drops_observed"]) * room_size
        loot_smoke_valid = not loot_smoke or (
            result["tcp_monster_death_received"] == expected_participant_cycles and
            result["tcp_drop_list_snapshot_received"] == expected_participant_cycles and
            loot_fanout_target >= expected_participant_cycles and
            result["rudp_click_loot_sent"] == loot_fanout_target and
            result["rudp_loot_resolved_received"] == loot_resolved_fanout_target
        )
        rudp_valid = udp_port is None or (
            result["battle_start_received"] == expected_participant_cycles and
            result["rudp_ack_sent"] == expected_participant_cycles
        )
        battle_cycles_valid = battle_cycles == 1 or (
            result["tcp_battle_final_ranking_received"] == expected_participant_cycles and
            result["tcp_lobby_return_received"] == expected_participant_cycles and
            result["tcp_room_list_snapshot_received"] >= expected_participant_cycles
        )
        result["valid"] = (
            room_fill_valid and
            tcp_battle_valid and
            arena_entry_valid and
            monster_spawn_valid and
            health_snapshot_valid and
            loot_smoke_valid and
            rudp_valid and
            battle_cycles_valid
        )
        if result["valid"]:
            result["reason"] = ""
        elif not room_fill_valid:
            result["reason"] = "room_fill_incomplete"
        elif not tcp_battle_valid:
            result["reason"] = "tcp_battle_start_incomplete"
        elif not arena_entry_valid:
            result["reason"] = "tcp_arena_entry_incomplete"
        elif not monster_spawn_valid:
            result["reason"] = "tcp_monster_spawn_incomplete"
        elif not health_snapshot_valid:
            result["reason"] = "tcp_monster_health_snapshot_incomplete"
        elif not loot_smoke_valid:
            result["reason"] = "rudp_loot_smoke_incomplete"
        elif not battle_cycles_valid:
            result["reason"] = "battle_cycle_result_incomplete"
        else:
            result["reason"] = "rudp_ready_incomplete"
    except (OSError, ValueError, TimeoutError) as error:
        result["valid"] = False
        result["reason"] = str(error)
        result["failure_stage"] = failure_stage
        result["failure_cycle"] = failure_cycle
        result["failure_room_first"] = failure_room_first
        result["failure_join_offset"] = failure_join_offset
        result["failure_error_type"] = type(error).__name__
        result["failure_error_errno"] = getattr(error, "errno", None)
        expected_tcp_packet_type = getattr(error, "expected_tcp_packet_type", None)
        tcp_packet_type = getattr(error, "tcp_packet_type", None)
        tcp_last_packet_type = getattr(error, "tcp_last_packet_type", None)
        result["failure_expected_tcp_packet_type"] = expected_tcp_packet_type
        result["failure_expected_tcp_packet_name"] = _tcp_packet_type_name(
            expected_tcp_packet_type,
        )
        result["failure_tcp_recv_phase"] = getattr(error, "tcp_recv_phase", None)
        result["failure_tcp_packet_type"] = tcp_packet_type
        result["failure_tcp_packet_name"] = _tcp_packet_type_name(tcp_packet_type)
        result["failure_tcp_ignored_packet_count"] = getattr(
            error,
            "tcp_ignored_packet_count",
            None,
        )
        result["failure_tcp_last_packet_type"] = tcp_last_packet_type
        result["failure_tcp_last_packet_name"] = _tcp_packet_type_name(
            tcp_last_packet_type,
        )
        result["failure_client_index"] = failure_client_index
        result["failure_client_id"] = failure_client_id
        result["failure_session_id"] = failure_session_id
        result["failure_connected_clients"] = len(clients)
    finally:
        for client in clients:
            client["tcp"].close()
            if "udp" in client:
                client["udp"].close()

    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run external Release 1 TCP room-fill workload."
    )
    parser.add_argument("--host", required=True)
    parser.add_argument("--tcp-port", required=True, type=int)
    parser.add_argument("--participants", required=True, type=int)
    parser.add_argument("--room-size", default=10, type=int)
    parser.add_argument("--timeout-sec", default=5.0, type=float)
    parser.add_argument("--udp-port", type=int)
    parser.add_argument("--auth-token-prefix")
    parser.add_argument("--start-battle", action="store_true")
    parser.add_argument("--loot-smoke", action="store_true")
    parser.add_argument("--move-duration-sec", default=0.0, type=float)
    parser.add_argument("--move-rate-hz", default=0.0, type=float)
    parser.add_argument("--attack-duration-sec", default=0.0, type=float)
    parser.add_argument("--attack-rate-hz", default=0.0, type=float)
    parser.add_argument("--battle-cycles", default=1, type=int)
    parser.add_argument("--client-id-base", default=FIRST_CLIENT_ID, type=int)
    parser.add_argument("--rudp-handshake-settle-sec", default=0.0, type=float)
    parser.add_argument("--output")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = run_external_tcp_room_fill(
            host=args.host,
            tcp_port=args.tcp_port,
            participants=args.participants,
            room_size=args.room_size,
            timeout_sec=args.timeout_sec,
            udp_port=args.udp_port,
            auth_token_prefix=args.auth_token_prefix,
            start_battle=args.start_battle,
            loot_smoke=args.loot_smoke,
            move_duration_sec=args.move_duration_sec,
            move_rate_hz=args.move_rate_hz,
            attack_duration_sec=args.attack_duration_sec,
            attack_rate_hz=args.attack_rate_hz,
            battle_cycles=args.battle_cycles,
            client_id_base=args.client_id_base,
            rudp_handshake_settle_sec=args.rudp_handshake_settle_sec,
        )
        _write_json(result, args.output)
    except (OSError, ValueError) as error:
        print(f"external stress runner failed: {error}", file=sys.stderr)
        return 2

    return 0 if result["valid"] else 1


def _base_result(
    host: str,
    tcp_port: int,
    participants: int,
    room_size: int,
    udp_port: int | None,
    auth_token_prefix: str | None,
    start_battle: bool,
    loot_smoke: bool,
    move_duration_sec: float,
    move_rate_hz: float,
    attack_duration_sec: float,
    attack_rate_hz: float,
    battle_cycles: int,
    client_id_base: int,
    rudp_handshake_settle_sec: float,
) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "workload": WORKLOAD,
        "target": {
            "host": host,
            "tcp_port": tcp_port,
            "udp_port": udp_port,
        },
        "requested_participants": participants,
        "room_size": room_size,
        "auth_enabled": auth_token_prefix is not None,
        "auth_sent": 0,
        "start_battle": start_battle,
        "loot_smoke": loot_smoke,
        "move_duration_sec": move_duration_sec,
        "move_rate_hz": move_rate_hz,
        "attack_duration_sec": attack_duration_sec,
        "attack_rate_hz": attack_rate_hz,
        "battle_cycles": battle_cycles,
        "client_id_base": client_id_base,
        "rudp_handshake_settle_sec": rudp_handshake_settle_sec,
        "tcp_ready_sent": 0,
        "tcp_ready_responses": 0,
        "tcp_host_start_sent": 0,
        "tcp_host_start_responses": 0,
        "tcp_battle_start_received": 0,
        "tcp_battle_start_roster_received": 0,
        "tcp_battle_load_entry_received": 0,
        "tcp_arena_load_complete_sent": 0,
        "tcp_arena_gameplay_start_received": 0,
        "tcp_monster_spawn_received": 0,
        "spawned_monsters": [],
        "rudp_move_target_sent": 0,
        "rudp_move_sent": 0,
        "rudp_attack_target_sent": 0,
        "rudp_attack_sent": 0,
        "tcp_monster_health_snapshot_received": 0,
        "monster_health_snapshots": [],
        "tcp_monster_death_received": 0,
        "tcp_drop_list_snapshot_received": 0,
        "drops_observed": [],
        "rudp_click_loot_target_sent": 0,
        "rudp_click_loot_sent": 0,
        "rudp_click_loot_server_accept_target_sent": 0,
        "rudp_loot_resolved_received": 0,
        "rudp_loot_timeout_context": None,
        "loot_resolved_events": [],
        "tcp_battle_final_ranking_received": 0,
        "tcp_lobby_return_received": 0,
        "tcp_room_list_snapshot_received": 0,
        "tcp_inventory_snapshot_received": 0,
        "tcp_loot_resolved_received": 0,
        "tcp_room_status_ignored_packets": 0,
        "tcp_room_status_ignored_room_list_snapshot": 0,
        "tcp_room_status_max_ignored_packets_before_response": 0,
        "tcp_room_status_max_ignored_room_list_snapshot_before_response": 0,
        "connected": 0,
        "welcome_received": 0,
        "rooms_created": 0,
        "rooms_filled": 0,
        "rudp_hello_sent": 0,
        "rudp_ready_sent": 0,
        "battle_start_received": 0,
        "rudp_ack_sent": 0,
        "valid": False,
        "reason": "",
    }


def _validate_config(
    host: str,
    tcp_port: int,
    participants: int,
    room_size: int,
    timeout_sec: float,
    udp_port: int | None,
    auth_token_prefix: str | None,
    start_battle: bool,
    loot_smoke: bool,
    move_duration_sec: float,
    move_rate_hz: float,
    attack_duration_sec: float,
    attack_rate_hz: float,
    battle_cycles: int,
    client_id_base: int,
    rudp_handshake_settle_sec: float,
) -> None:
    if not host.strip():
        raise ValueError("host is required")
    if tcp_port < 1 or tcp_port > 65535:
        raise ValueError("tcp_port must be between 1 and 65535")
    if participants <= 0:
        raise ValueError("participants must be positive")
    if room_size < 2 or room_size > 10:
        raise ValueError("room_size must be between 2 and 10")
    if participants % room_size != 0:
        raise ValueError("participants must be divisible by room_size")
    if timeout_sec <= 0:
        raise ValueError("timeout_sec must be positive")
    if udp_port is not None and (udp_port < 1 or udp_port > 65535):
        raise ValueError("udp_port must be between 1 and 65535")
    if auth_token_prefix is not None and not auth_token_prefix.strip():
        raise ValueError("auth_token_prefix must not be blank")
    if start_battle and room_size < 2:
        raise ValueError("start_battle requires room_size >= 2")
    if loot_smoke and (not start_battle or udp_port is None):
        raise ValueError("loot_smoke requires start_battle and udp_port")
    if move_duration_sec < 0:
        raise ValueError("move_duration_sec must be non-negative")
    if move_rate_hz < 0:
        raise ValueError("move_rate_hz must be non-negative")
    if (move_duration_sec > 0) != (move_rate_hz > 0):
        raise ValueError("move_duration_sec and move_rate_hz must be set together")
    if move_duration_sec > 0 and (not start_battle or udp_port is None):
        raise ValueError("move workload requires start_battle and udp_port")
    if attack_duration_sec < 0:
        raise ValueError("attack_duration_sec must be non-negative")
    if attack_rate_hz < 0:
        raise ValueError("attack_rate_hz must be non-negative")
    if (attack_duration_sec > 0) != (attack_rate_hz > 0):
        raise ValueError("attack_duration_sec and attack_rate_hz must be set together")
    if attack_duration_sec > 0 and (not start_battle or udp_port is None):
        raise ValueError("attack workload requires start_battle and udp_port")
    if attack_duration_sec > 0 and not loot_smoke:
        raise ValueError("attack workload currently requires loot_smoke")
    if battle_cycles <= 0:
        raise ValueError("battle_cycles must be positive")
    if battle_cycles > 1 and (not loot_smoke or not start_battle or udp_port is None):
        raise ValueError("battle_cycles greater than 1 requires loot_smoke")
    if client_id_base <= 0:
        raise ValueError("client_id_base must be positive")
    if client_id_base + participants - 1 > 0xFFFFFFFF:
        raise ValueError("client_id_base range must fit in uint32")
    if rudp_handshake_settle_sec < 0:
        raise ValueError("rudp_handshake_settle_sec must be non-negative")


def _move_iterations(move_duration_sec: float, move_rate_hz: float) -> int:
    if move_duration_sec <= 0 or move_rate_hz <= 0:
        return 1
    return max(1, int(round(move_duration_sec * move_rate_hz)))


def _attack_rounds(
    attack_duration_sec: float,
    attack_rate_hz: float,
    loot_smoke: bool,
) -> int:
    if attack_duration_sec <= 0 or attack_rate_hz <= 0:
        return LOOT_SMOKE_ATTACK_ROUNDS if loot_smoke else 1
    return max(1, int(round(attack_duration_sec * attack_rate_hz)))


def _first_ready_cmd_seq(
    cycle_index: int,
    move_iterations: int,
    attack_rounds: int,
    loot_smoke: bool,
    room_size: int,
) -> int:
    loot_command_slots = max(1, room_size - 1) if loot_smoke else 0
    command_count = 1 + move_iterations + attack_rounds + loot_command_slots
    return 1 + (cycle_index * command_count)


def _receive_welcome(client: socket.socket) -> int:
    packet_type, payload = _recv_packet(client)
    if packet_type != TYPE_WELCOME or len(payload) != 8:
        raise ValueError("expected Welcome packet")
    return struct.unpack(">Q", payload)[0]


def _create_room(
    client: socket.socket,
    max_players: int,
    timeout_sec: float,
    result: dict[str, Any],
) -> int:
    client.sendall(_create_room_request("Room", max_players))
    room_id, player_count = _recv_room_status(
        client,
        TYPE_CREATE_ROOM_RESPONSE,
        timeout_sec,
        result,
    )
    if room_id == 0 or player_count != 1:
        raise ValueError("invalid CreateRoomResponse")
    return room_id


def _join_room(
    client: socket.socket,
    room_id: int,
    timeout_sec: float,
    result: dict[str, Any],
) -> int:
    client.sendall(_join_room_request(room_id))
    joined_room_id, player_count = _recv_room_status(
        client,
        TYPE_JOIN_ROOM_RESPONSE,
        timeout_sec,
        result,
    )
    if joined_room_id != room_id:
        raise ValueError("invalid JoinRoomResponse room_id")
    return player_count


def _run_rudp_ready_smoke(
    host: str,
    udp_port: int,
    room_id: int,
    room_clients: list[dict[str, Any]],
    timeout_sec: float,
    result: dict[str, Any],
) -> None:
    _send_rudp_hellos(host, udp_port, room_clients, result)
    _send_rudp_ready_and_ack_battle_start(
        host,
        udp_port,
        room_id,
        room_clients,
        timeout_sec,
        result,
    )


def _send_rudp_hellos(
    host: str,
    udp_port: int,
    room_clients: list[dict[str, Any]],
    result: dict[str, Any],
) -> None:
    target = (host, udp_port)
    for client in room_clients:
        client["udp"].sendto(
            _rudp_hello_packet(client["client_id"], client["session_id"]),
            target,
        )
        result["rudp_hello_sent"] += 1


def _send_rudp_ready_and_ack_battle_start(
    host: str,
    udp_port: int,
    room_id: int,
    room_clients: list[dict[str, Any]],
    timeout_sec: float,
    result: dict[str, Any],
    ready_cmd_seq: int = 1,
) -> None:
    target = (host, udp_port)
    for client in room_clients:
        client["udp"].sendto(
            _rudp_ready_packet(client["client_id"], ready_cmd_seq),
            target,
        )
        result["rudp_ready_sent"] += 1

    for index, client in enumerate(room_clients):
        header = _recv_battle_start(
            client["udp"],
            room_id,
            client["session_id"],
            timeout_sec,
        )
        result["battle_start_received"] += 1
        client["udp"].sendto(
            _rudp_ack_packet(header["sequence"], 200000 + index),
            target,
        )
        result["rudp_ack_sent"] += 1


def _send_tcp_start_battle(
    room_clients: list[dict[str, Any]],
    result: dict[str, Any],
    timeout_sec: float,
) -> dict[str, int] | None:
    for client in room_clients:
        client["tcp"].sendall(_header_only_packet(TYPE_READY_ROOM_REQUEST))
        result["tcp_ready_sent"] += 1

    for client in room_clients:
        _recv_ready_room_response(client["tcp"], len(room_clients), timeout_sec)
        result["tcp_ready_responses"] += 1

    room_clients[0]["tcp"].sendall(_header_only_packet(TYPE_HOST_START_BATTLE_REQUEST))
    result["tcp_host_start_sent"] += 1
    room_id = _recv_room_id_response(
        room_clients[0]["tcp"],
        TYPE_HOST_START_BATTLE_RESPONSE,
        timeout_sec,
    )
    result["tcp_host_start_responses"] += 1
    _recv_tcp_battle_start_fanout(room_clients, room_id, result, timeout_sec)
    battle_instance_id = _recv_battle_load_entry_fanout(
        room_clients,
        room_id,
        result,
        timeout_sec,
    )
    _send_arena_load_complete_fanout(
        room_clients,
        room_id,
        battle_instance_id,
        result,
    )
    _recv_arena_gameplay_start_fanout(
        room_clients,
        room_id,
        battle_instance_id,
        result,
        timeout_sec,
    )
    return _recv_monster_spawn_fanout(room_clients, room_id, result, timeout_sec)


def _send_rudp_attack_and_recv_health_snapshot(
    host: str,
    udp_port: int,
    room_clients: list[dict[str, Any]],
    spawned_monster: dict[str, int],
    timeout_sec: float,
    result: dict[str, Any],
    cmd_seq: int,
) -> None:
    target = (host, udp_port)
    result["rudp_attack_target_sent"] += len(room_clients)
    for client in room_clients:
        client["udp"].sendto(
            _rudp_attack_packet(
                client["client_id"],
                spawned_monster["monster_id"],
                cmd_seq,
            ),
            target,
        )
        result["rudp_attack_sent"] += 1

    _recv_monster_health_snapshot_fanout(
        room_clients,
        spawned_monster,
        result,
        timeout_sec,
    )


def _send_rudp_attack_until_drop_and_click_loot(
    host: str,
    udp_port: int,
    room_clients: list[dict[str, Any]],
    spawned_monster: dict[str, int],
    timeout_sec: float,
    result: dict[str, Any],
    first_attack_cmd_seq: int,
    attack_rounds: int,
) -> None:
    target = (host, udp_port)
    result["rudp_attack_target_sent"] += len(room_clients) * attack_rounds
    for attack_round in range(attack_rounds):
        cmd_seq = first_attack_cmd_seq + attack_round
        for client in room_clients:
            client["udp"].sendto(
                _rudp_attack_packet(
                    client["client_id"],
                    spawned_monster["monster_id"],
                    cmd_seq,
                ),
                target,
            )
            result["rudp_attack_sent"] += 1

    drops = _recv_monster_death_and_drop_fanout(
        room_clients,
        spawned_monster,
        result,
        timeout_sec,
    )
    for drop_index, drop in enumerate(drops):
        click_cmd_seq = first_attack_cmd_seq + attack_rounds + drop_index
        click_clients = room_clients if drop_index == 0 else room_clients[:1]
        result["rudp_click_loot_target_sent"] += len(click_clients)
        result["rudp_click_loot_server_accept_target_sent"] += (
            1 if len(drops) == 1 else len(click_clients)
        )
        for client in click_clients:
            client["udp"].sendto(
                _rudp_click_loot_packet(
                    client["client_id"],
                    drop["drop_id"],
                    click_cmd_seq,
                ),
                target,
            )
            result["rudp_click_loot_sent"] += 1

        _recv_rudp_loot_resolved_fanout(
            target,
            room_clients,
            drop,
            result,
            timeout_sec,
            drop_index=drop_index,
            drop_count=len(drops),
        )


def _send_rudp_move_workload(
    host: str,
    udp_port: int,
    room_clients: list[dict[str, Any]],
    result: dict[str, Any],
    move_iterations: int,
    move_rate_hz: float,
    first_cmd_seq: int,
) -> int:
    target = (host, udp_port)
    result["rudp_move_target_sent"] += len(room_clients) * move_iterations
    next_send_at = time.monotonic()
    interval_sec = 1.0 / move_rate_hz if move_rate_hz > 0 else 0.0
    for move_index in range(move_iterations):
        cmd_seq = first_cmd_seq + move_index
        for client in room_clients:
            client["udp"].sendto(
                _rudp_move_packet(
                    client["client_id"],
                    dir_x=100,
                    dir_y=0,
                    cmd_seq=cmd_seq,
                ),
                target,
            )
            result["rudp_move_sent"] += 1
        if interval_sec > 0 and move_index + 1 < move_iterations:
            next_send_at += interval_sec
            time.sleep(max(0.0, next_send_at - time.monotonic()))
    return first_cmd_seq + move_iterations


def _recv_room_status(
    client: socket.socket,
    expected_type: int,
    timeout_sec: float,
    result: dict[str, Any],
) -> tuple[int, int]:
    client.settimeout(timeout_sec)
    ignored_packet_count = 0
    ignored_room_list_snapshot_count = 0
    last_packet_type: int | None = None
    while True:
        try:
            packet_type, payload = _recv_packet(client)
        except ConnectionError as error:
            _record_room_status_ignored_packets(
                result,
                ignored_packet_count,
                ignored_room_list_snapshot_count,
            )
            _annotate_tcp_receive_error(
                error,
                expected_type,
                ignored_packet_count,
                last_packet_type,
            )
            raise
        if packet_type != expected_type:
            ignored_packet_count += 1
            last_packet_type = packet_type
            if packet_type == TYPE_ROOM_LIST_SNAPSHOT:
                ignored_room_list_snapshot_count += 1
            continue
        if len(payload) != 6:
            raise ValueError("invalid room status packet")
        _record_room_status_ignored_packets(
            result,
            ignored_packet_count,
            ignored_room_list_snapshot_count,
        )
        return struct.unpack(">IH", payload)


def _recv_ready_room_response(
    client: socket.socket,
    expected_total_players: int,
    timeout_sec: float,
) -> tuple[int, int, int]:
    client.settimeout(timeout_sec)
    while True:
        packet_type, payload = _recv_packet(client)
        _raise_on_tcp_error(packet_type, payload, "ReadyRoomRequest")
        if packet_type != TYPE_READY_ROOM_RESPONSE:
            continue
        if len(payload) != 8:
            raise ValueError("invalid ReadyRoomResponse")
        room_id, ready_count, total_count = struct.unpack(">IHH", payload)
        if room_id == 0 or total_count != expected_total_players:
            raise ValueError("invalid ReadyRoomResponse state")
        if ready_count == 0 or ready_count > total_count:
            raise ValueError("invalid ReadyRoomResponse ready_count")
        return room_id, ready_count, total_count


def _recv_room_id_response(
    client: socket.socket,
    expected_type: int,
    timeout_sec: float,
) -> int:
    client.settimeout(timeout_sec)
    while True:
        packet_type, payload = _recv_packet(client)
        _raise_on_tcp_error(packet_type, payload, "HostStartBattleRequest")
        if packet_type != expected_type:
            continue
        if len(payload) != 4:
            raise ValueError("invalid room id response")
        room_id = struct.unpack(">I", payload)[0]
        if room_id == 0:
            raise ValueError("invalid room id response")
        return room_id


def _recv_tcp_battle_start_fanout(
    room_clients: list[dict[str, Any]],
    expected_room_id: int,
    result: dict[str, Any],
    timeout_sec: float,
) -> None:
    expected_players = len(room_clients)
    for client in room_clients:
        if expected_players == 2:
            _recv_battle_start_packet(client["tcp"], expected_room_id, timeout_sec)
            result["tcp_battle_start_received"] += 1
        else:
            _recv_battle_start_roster_packet(
                client["tcp"],
                expected_room_id,
                expected_players,
                timeout_sec,
            )
            result["tcp_battle_start_roster_received"] += 1


def _recv_battle_start_packet(
    client: socket.socket,
    expected_room_id: int,
    timeout_sec: float,
) -> tuple[int, int, int]:
    client.settimeout(timeout_sec)
    while True:
        packet_type, payload = _recv_packet(client)
        _raise_on_tcp_error(packet_type, payload, "BattleStart")
        if packet_type != TYPE_BATTLE_START:
            continue
        if len(payload) != 20:
            raise ValueError("invalid BattleStart")
        room_id, session_a, session_b = struct.unpack(">IQQ", payload)
        if room_id != expected_room_id or session_a == 0 or session_b == 0:
            raise ValueError("invalid BattleStart state")
        if session_a == session_b:
            raise ValueError("invalid BattleStart duplicate sessions")
        return room_id, session_a, session_b


def _recv_battle_start_roster_packet(
    client: socket.socket,
    expected_room_id: int,
    expected_players: int,
    timeout_sec: float,
) -> tuple[int, list[int]]:
    client.settimeout(timeout_sec)
    while True:
        packet_type, payload = _recv_packet(client)
        _raise_on_tcp_error(packet_type, payload, "BattleStartRoster")
        if packet_type != TYPE_BATTLE_START_ROSTER:
            continue
        if len(payload) < 6:
            raise ValueError("invalid BattleStartRoster")
        room_id, player_count = struct.unpack(">IH", payload[:6])
        expected_size = 6 + (player_count * 8)
        if len(payload) != expected_size:
            raise ValueError("invalid BattleStartRoster size")
        if room_id != expected_room_id or player_count != expected_players:
            raise ValueError("invalid BattleStartRoster state")
        session_ids = [
            struct.unpack(">Q", payload[6 + (index * 8):14 + (index * 8)])[0]
            for index in range(player_count)
        ]
        if any(session_id == 0 for session_id in session_ids):
            raise ValueError("invalid BattleStartRoster zero session")
        if len(set(session_ids)) != len(session_ids):
            raise ValueError("invalid BattleStartRoster duplicate sessions")
        return room_id, session_ids


def _recv_battle_load_entry_fanout(
    room_clients: list[dict[str, Any]],
    expected_room_id: int,
    result: dict[str, Any],
    timeout_sec: float,
) -> int:
    expected_players = len(room_clients)
    battle_instance_id = 0
    for client in room_clients:
        room_id, current_battle_instance_id, session_ids = _recv_battle_load_entry_packet(
            client["tcp"],
            expected_room_id,
            expected_players,
            timeout_sec,
        )
        if room_id != expected_room_id or len(session_ids) != expected_players:
            raise ValueError("invalid BattleLoadEntry state")
        if battle_instance_id == 0:
            battle_instance_id = current_battle_instance_id
        elif battle_instance_id != current_battle_instance_id:
            raise ValueError("inconsistent BattleLoadEntry battle_instance_id")
        result["tcp_battle_load_entry_received"] += 1
    return battle_instance_id


def _recv_battle_load_entry_packet(
    client: socket.socket,
    expected_room_id: int,
    expected_players: int,
    timeout_sec: float,
) -> tuple[int, int, list[int]]:
    client.settimeout(timeout_sec)
    while True:
        packet_type, payload = _recv_packet(client)
        _raise_on_tcp_error(packet_type, payload, "BattleLoadEntry")
        if packet_type != TYPE_BATTLE_LOAD_ENTRY:
            continue
        if len(payload) < 14:
            raise ValueError("invalid BattleLoadEntry")
        room_id, battle_instance_id, player_count = struct.unpack(">IQH", payload[:14])
        expected_size = 14 + (player_count * 8)
        if len(payload) != expected_size:
            raise ValueError("invalid BattleLoadEntry size")
        if room_id != expected_room_id or battle_instance_id == 0:
            raise ValueError("invalid BattleLoadEntry state")
        if player_count != expected_players:
            raise ValueError("invalid BattleLoadEntry player_count")
        session_ids = [
            struct.unpack(">Q", payload[14 + (index * 8):22 + (index * 8)])[0]
            for index in range(player_count)
        ]
        if any(session_id == 0 for session_id in session_ids):
            raise ValueError("invalid BattleLoadEntry zero session")
        if len(set(session_ids)) != len(session_ids):
            raise ValueError("invalid BattleLoadEntry duplicate sessions")
        return room_id, battle_instance_id, session_ids


def _send_arena_load_complete_fanout(
    room_clients: list[dict[str, Any]],
    room_id: int,
    battle_instance_id: int,
    result: dict[str, Any],
) -> None:
    packet = _packet(TYPE_ARENA_LOAD_COMPLETE, struct.pack(">IQ", room_id, battle_instance_id))
    for client in room_clients:
        client["tcp"].sendall(packet)
        result["tcp_arena_load_complete_sent"] += 1


def _recv_arena_gameplay_start_fanout(
    room_clients: list[dict[str, Any]],
    expected_room_id: int,
    expected_battle_instance_id: int,
    result: dict[str, Any],
    timeout_sec: float,
) -> None:
    for client in room_clients:
        _recv_arena_gameplay_start_packet(
            client["tcp"],
            expected_room_id,
            expected_battle_instance_id,
            timeout_sec,
        )
        result["tcp_arena_gameplay_start_received"] += 1


def _recv_arena_gameplay_start_packet(
    client: socket.socket,
    expected_room_id: int,
    expected_battle_instance_id: int,
    timeout_sec: float,
) -> tuple[int, int]:
    client.settimeout(timeout_sec)
    while True:
        packet_type, payload = _recv_packet(client)
        _raise_on_tcp_error(packet_type, payload, "ArenaGameplayStart")
        if packet_type != TYPE_ARENA_GAMEPLAY_START:
            continue
        if len(payload) != 12:
            raise ValueError("invalid ArenaGameplayStart")
        room_id, battle_instance_id = struct.unpack(">IQ", payload)
        if room_id != expected_room_id or battle_instance_id != expected_battle_instance_id:
            raise ValueError("invalid ArenaGameplayStart state")
        return room_id, battle_instance_id


def _recv_monster_spawn_fanout(
    room_clients: list[dict[str, Any]],
    expected_room_id: int,
    result: dict[str, Any],
    timeout_sec: float,
) -> dict[str, int] | None:
    expected_spawn: dict[str, int] | None = None
    for client in room_clients:
        spawn = _recv_monster_spawn_packet(client["tcp"], expected_room_id, timeout_sec)
        if expected_spawn is None:
            expected_spawn = spawn
        elif spawn != expected_spawn:
            raise ValueError("inconsistent MonsterSpawn fanout")
        result["tcp_monster_spawn_received"] += 1
    if expected_spawn is not None:
        result["spawned_monsters"].append(expected_spawn)
    return expected_spawn


def _recv_monster_spawn_packet(
    client: socket.socket,
    expected_room_id: int,
    timeout_sec: float,
) -> dict[str, int]:
    client.settimeout(timeout_sec)
    while True:
        packet_type, payload = _recv_packet(client)
        _raise_on_tcp_error(packet_type, payload, "MonsterSpawn")
        if packet_type != TYPE_MONSTER_SPAWN:
            continue
        if len(payload) != 14:
            raise ValueError("invalid MonsterSpawn")
        room_id, monster_id, monster_type_id, max_hp = struct.unpack(">IIIH", payload)
        if room_id != expected_room_id or monster_id == 0 or monster_type_id == 0 or max_hp == 0:
            raise ValueError("invalid MonsterSpawn state")
        return {
            "room_id": room_id,
            "monster_id": monster_id,
            "monster_type_id": monster_type_id,
            "max_hp": max_hp,
        }


def _recv_monster_health_snapshot_fanout(
    room_clients: list[dict[str, Any]],
    expected_spawn: dict[str, int],
    result: dict[str, Any],
    timeout_sec: float,
) -> None:
    expected_snapshot: dict[str, int] | None = None
    for client in room_clients:
        snapshot = _recv_monster_health_snapshot_packet(
            client["tcp"],
            expected_spawn,
            timeout_sec,
        )
        if expected_snapshot is None:
            expected_snapshot = snapshot
        elif snapshot != expected_snapshot:
            raise ValueError("inconsistent MonsterHealthSnapshot fanout")
        result["tcp_monster_health_snapshot_received"] += 1
    if expected_snapshot is not None:
        result["monster_health_snapshots"].append(expected_snapshot)


def _recv_monster_health_snapshot_packet(
    client: socket.socket,
    expected_spawn: dict[str, int],
    timeout_sec: float,
) -> dict[str, int]:
    client.settimeout(timeout_sec)
    while True:
        packet_type, payload = _recv_packet(client)
        _raise_on_tcp_error(packet_type, payload, "MonsterHealthSnapshot")
        if packet_type != TYPE_MONSTER_HEALTH_SNAPSHOT:
            continue
        if len(payload) != 12:
            raise ValueError("invalid MonsterHealthSnapshot")
        room_id, monster_id, current_hp, max_hp = struct.unpack(">IIHH", payload)
        if room_id != expected_spawn["room_id"] or monster_id != expected_spawn["monster_id"]:
            raise ValueError("invalid MonsterHealthSnapshot state")
        if max_hp != expected_spawn["max_hp"] or current_hp >= max_hp:
            raise ValueError("invalid MonsterHealthSnapshot hp")
        return {
            "room_id": room_id,
            "monster_id": monster_id,
            "current_hp": current_hp,
            "max_hp": max_hp,
        }


def _recv_monster_death_and_drop_fanout(
    room_clients: list[dict[str, Any]],
    expected_spawn: dict[str, int],
    result: dict[str, Any],
    timeout_sec: float,
) -> list[dict[str, int]]:
    expected_drops: list[dict[str, int]] | None = None
    for client in room_clients:
        client["tcp"].settimeout(timeout_sec)
        saw_death = False
        while True:
            packet_type, payload = _recv_packet(client["tcp"])
            _raise_on_tcp_error(packet_type, payload, "LootSmoke")
            if packet_type == TYPE_MONSTER_HEALTH_SNAPSHOT:
                continue
            if packet_type == TYPE_MONSTER_DEATH:
                _parse_monster_death_payload(payload, expected_spawn)
                result["tcp_monster_death_received"] += 1
                saw_death = True
                continue
            if packet_type != TYPE_DROP_LIST_SNAPSHOT_V2:
                continue
            if not saw_death:
                raise ValueError("DropListSnapshotV2 arrived before MonsterDeath")
            drops = _parse_drop_list_snapshot_v2_payload(payload, expected_spawn["room_id"])
            if expected_drops is None:
                expected_drops = drops
                result["drops_observed"].extend(drops)
            elif drops != expected_drops:
                raise ValueError("inconsistent DropListSnapshotV2 fanout")
            result["tcp_drop_list_snapshot_received"] += 1
            break

    if expected_drops is None:
        raise ValueError("missing DropListSnapshotV2")
    return expected_drops


def _recv_rudp_loot_resolved_fanout(
    target: tuple[str, int],
    room_clients: list[dict[str, Any]],
    expected_drop: dict[str, int],
    result: dict[str, Any],
    timeout_sec: float,
    *,
    drop_index: int,
    drop_count: int,
) -> None:
    expected_resolution: dict[str, int] | None = None
    for index, client in enumerate(room_clients):
        try:
            header, resolution = _recv_rudp_loot_resolved_event(
                client["udp"],
                expected_drop,
                timeout_sec,
            )
        except socket.timeout:
            result["rudp_loot_timeout_context"] = {
                "room_id": expected_drop["room_id"],
                "drop_id": expected_drop["drop_id"],
                "item_id": expected_drop["item_id"],
                "quantity": expected_drop["quantity"],
                "drop_index": drop_index,
                "drop_count": drop_count,
                "expected_fanout": len(room_clients),
                "received_fanout": index,
                "timed_out_client_index": index,
                "timed_out_client_id": client.get("client_id"),
            }
            raise
        if expected_resolution is None:
            expected_resolution = resolution
            result["loot_resolved_events"].append(resolution)
        elif resolution != expected_resolution:
            raise ValueError("inconsistent RUDP LootResolved fanout")
        result["rudp_loot_resolved_received"] += 1
        client["udp"].sendto(
            _rudp_ack_packet(header["sequence"], 210000 + index),
            target,
        )


def _recv_battle_result_and_lobby_return_fanout(
    room_clients: list[dict[str, Any]],
    expected_room_id: int,
    timeout_sec: float,
    result: dict[str, Any],
) -> None:
    for client in room_clients:
        client["tcp"].settimeout(timeout_sec)
        saw_final_ranking = False
        saw_lobby_return = False
        saw_room_list = False
        while not (saw_final_ranking and saw_lobby_return and saw_room_list):
            packet_type, payload = _recv_packet(client["tcp"])
            _raise_on_tcp_error(packet_type, payload, "BattleCycleResult")
            if packet_type == TYPE_LOOT_RESOLVED:
                _parse_tcp_loot_resolved_payload(payload, expected_room_id)
                result["tcp_loot_resolved_received"] += 1
                continue
            if packet_type == TYPE_INVENTORY_SNAPSHOT:
                _parse_inventory_snapshot_payload(payload)
                result["tcp_inventory_snapshot_received"] += 1
                continue
            if packet_type == TYPE_BATTLE_FINAL_RANKING:
                _parse_battle_final_ranking_payload(payload, expected_room_id)
                result["tcp_battle_final_ranking_received"] += 1
                saw_final_ranking = True
                continue
            if packet_type == TYPE_LOBBY_RETURN_VISIBILITY:
                _parse_lobby_return_visibility_payload(payload, expected_room_id)
                result["tcp_lobby_return_received"] += 1
                saw_lobby_return = True
                continue
            if packet_type == TYPE_ROOM_LIST_SNAPSHOT:
                _parse_room_list_snapshot_payload(payload)
                result["tcp_room_list_snapshot_received"] += 1
                saw_room_list = True


def _parse_tcp_loot_resolved_payload(payload: bytes, expected_room_id: int) -> None:
    if len(payload) != 22:
        raise ValueError("invalid LootResolved")
    room_id, drop_id, winner_session_id, item_id, quantity = struct.unpack(">IIQIH", payload)
    if (
        room_id != expected_room_id or
        drop_id == 0 or
        winner_session_id == 0 or
        item_id == 0 or
        quantity == 0
    ):
        raise ValueError("invalid LootResolved state")


def _parse_inventory_snapshot_payload(payload: bytes) -> None:
    if len(payload) < 14:
        raise ValueError("invalid InventorySnapshot")
    session_id, _current_weight, _max_weight, entry_count = struct.unpack(">QHHH", payload[:14])
    expected_size = 14 + (entry_count * 6)
    if session_id == 0 or len(payload) != expected_size:
        raise ValueError("invalid InventorySnapshot state")


def _parse_lobby_return_visibility_payload(payload: bytes, expected_room_id: int) -> None:
    if len(payload) != 5:
        raise ValueError("invalid LobbyReturnVisibility")
    previous_room_id, reason = struct.unpack(">IB", payload)
    if previous_room_id != expected_room_id or reason != 0:
        raise ValueError("invalid LobbyReturnVisibility state")


def _parse_room_list_snapshot_payload(payload: bytes) -> None:
    if len(payload) < 2:
        raise ValueError("invalid RoomListSnapshot")
    room_count = struct.unpack(">H", payload[:2])[0]
    offset = 2
    for _ in range(room_count):
        if len(payload) < offset + 10:
            raise ValueError("invalid RoomListSnapshot entry")
        _room_id, _players, _max_players, _status, title_len = struct.unpack(
            ">IHHBB",
            payload[offset:offset + 10],
        )
        offset += 10 + title_len
        if len(payload) < offset:
            raise ValueError("invalid RoomListSnapshot title")
    if offset != len(payload):
        raise ValueError("invalid RoomListSnapshot trailing bytes")


def _parse_battle_final_ranking_payload(payload: bytes, expected_room_id: int) -> None:
    if len(payload) < 13:
        raise ValueError("invalid BattleFinalRanking")
    room_id, battle_instance_id, row_count = struct.unpack(">IQB", payload[:13])
    if room_id != expected_room_id or battle_instance_id == 0 or row_count == 0 or row_count > 10:
        raise ValueError("invalid BattleFinalRanking state")
    offset = 13
    session_ids: set[int] = set()
    for _ in range(row_count):
        if len(payload) < offset + 11:
            raise ValueError("invalid BattleFinalRanking row")
        rank, session_id, nickname_len = struct.unpack(">HQB", payload[offset:offset + 11])
        offset += 11
        if len(payload) < offset + nickname_len + 8:
            raise ValueError("invalid BattleFinalRanking nickname")
        nickname = payload[offset:offset + nickname_len]
        offset += nickname_len
        _total_asset_value = struct.unpack(">q", payload[offset:offset + 8])[0]
        offset += 8
        if rank == 0 or session_id == 0 or not nickname or session_id in session_ids:
            raise ValueError("invalid BattleFinalRanking row state")
        session_ids.add(session_id)
    if offset != len(payload):
        raise ValueError("invalid BattleFinalRanking trailing bytes")


def _parse_monster_death_payload(
    payload: bytes,
    expected_spawn: dict[str, int],
) -> dict[str, int]:
    if len(payload) != 8:
        raise ValueError("invalid MonsterDeath")
    room_id, monster_id = struct.unpack(">II", payload)
    if room_id != expected_spawn["room_id"] or monster_id != expected_spawn["monster_id"]:
        raise ValueError("invalid MonsterDeath state")
    return {
        "room_id": room_id,
        "monster_id": monster_id,
    }


def _parse_drop_list_snapshot_v2_payload(
    payload: bytes,
    expected_room_id: int,
) -> list[dict[str, int]]:
    if len(payload) < 10:
        raise ValueError("invalid DropListSnapshotV2")
    room_id, _scatter_seed, drop_count = struct.unpack(">IIH", payload[:10])
    expected_size = 10 + (drop_count * 18)
    if room_id != expected_room_id or drop_count == 0 or len(payload) != expected_size:
        raise ValueError("invalid DropListSnapshotV2 state")
    drops: list[dict[str, int]] = []
    offset = 10
    for _ in range(drop_count):
        drop_id, item_id, quantity, _pos_x, _pos_y = struct.unpack(
            ">IIHii",
            payload[offset:offset + 18],
        )
        if drop_id == 0 or item_id == 0 or quantity == 0:
            raise ValueError("invalid DropListSnapshotV2 drop")
        drops.append({
            "room_id": room_id,
            "drop_id": drop_id,
            "item_id": item_id,
            "quantity": quantity,
        })
        offset += 18
    return drops


def _recv_rudp_loot_resolved_event(
    udp_socket: socket.socket,
    expected_drop: dict[str, int],
    timeout_sec: float,
) -> tuple[dict[str, int], dict[str, int]]:
    udp_socket.settimeout(timeout_sec)
    while True:
        data, _ = udp_socket.recvfrom(MAX_RUDP_PACKET_SIZE)
        header, payload = _parse_rudp_packet(data)
        if header["packet_type"] != RUDP_TYPE_GAME_EVENT:
            continue
        resolution = _parse_rudp_loot_resolved_payload(payload, expected_drop)
        if resolution is None:
            continue
        return header, resolution


def _parse_rudp_loot_resolved_payload(
    payload: bytes,
    expected_drop: dict[str, int],
) -> dict[str, int] | None:
    if len(payload) != RUDP_LOOT_RESOLVED_PAYLOAD_SIZE:
        return None
    event_type, body_size = struct.unpack(">HH", payload[:4])
    if event_type != RUDP_GAME_EVENT_LOOT_RESOLVED or body_size != RUDP_LOOT_RESOLVED_BODY_SIZE:
        return None
    room_id, drop_id, winner_session_id, item_id, quantity = struct.unpack(">IIQIH", payload[4:])
    if (
        room_id != expected_drop["room_id"] or
        drop_id != expected_drop["drop_id"] or
        item_id != expected_drop["item_id"] or
        quantity != expected_drop["quantity"]
    ):
        return None
    if winner_session_id == 0:
        raise ValueError("invalid LootResolved state")
    return {
        "room_id": room_id,
        "drop_id": drop_id,
        "winner_session_id": winner_session_id,
        "item_id": item_id,
        "quantity": quantity,
    }


def _raise_on_tcp_error(packet_type: int, payload: bytes, context: str) -> None:
    if packet_type != TYPE_ERROR:
        return
    if len(payload) != 4:
        raise ValueError("invalid Error packet")
    failed_type, error_code = struct.unpack(">HH", payload)
    raise ValueError(f"{context} rejected type=0x{failed_type:04x} code={error_code}")


def _create_room_request(room_title: str, max_players: int) -> bytes:
    title_bytes = room_title.encode("utf-8")
    if len(title_bytes) == 0 or len(title_bytes) > 64:
        raise ValueError("room title must be 1..64 bytes")
    if max_players < 2 or max_players > 10:
        raise ValueError("max_players must be between 2 and 10")
    payload = struct.pack(">B", len(title_bytes)) + title_bytes + struct.pack(">B", max_players)
    return _packet(TYPE_CREATE_ROOM_REQUEST, payload)


def _join_room_request(room_id: int) -> bytes:
    return _packet(TYPE_JOIN_ROOM_REQUEST, struct.pack(">I", room_id))


def _header_only_packet(packet_type: int) -> bytes:
    return _packet(packet_type, b"")


def _authenticate_packet(token: str) -> bytes:
    token_bytes = token.encode("utf-8")
    if len(token_bytes) == 0 or len(token_bytes) > 512:
        raise ValueError("auth token must be 1..512 bytes")
    return _packet(TYPE_AUTHENTICATE_GAME_SESSION, struct.pack(">H", len(token_bytes)) + token_bytes)


def _auth_token(prefix: str, index: int) -> str:
    return f"{prefix}P{index:06d}"


def _rudp_hello_packet(client_id: int, session_id: int) -> bytes:
    payload = struct.pack(">HIQ", RUDP_CLIENT_VERSION, client_id, session_id)
    return _rudp_packet(
        channel=RUDP_CHANNEL_CONTROL,
        packet_type=RUDP_TYPE_HELLO,
        sequence=1000 + (client_id - FIRST_CLIENT_ID),
        payload=payload,
    )


def _rudp_ready_packet(client_id: int, cmd_seq: int = 1) -> bytes:
    payload = struct.pack(">IIBB", client_id, cmd_seq, RUDP_INPUT_READY, 0)
    return _rudp_packet(
        channel=RUDP_CHANNEL_INPUT,
        packet_type=RUDP_TYPE_INPUT_COMMAND,
        sequence=100000 + (client_id - FIRST_CLIENT_ID),
        payload=payload,
    )


def _rudp_attack_packet(client_id: int, monster_id: int, cmd_seq: int = 3) -> bytes:
    payload = struct.pack(">IIBBI", client_id, cmd_seq, RUDP_INPUT_ATTACK, 4, monster_id)
    return _rudp_packet(
        channel=RUDP_CHANNEL_INPUT,
        packet_type=RUDP_TYPE_INPUT_COMMAND,
        sequence=110000 + ((cmd_seq - 3) * 1000) + (client_id - FIRST_CLIENT_ID),
        payload=payload,
    )


def _rudp_click_loot_packet(client_id: int, drop_id: int, cmd_seq: int) -> bytes:
    payload = struct.pack(">IIBBI", client_id, cmd_seq, RUDP_INPUT_CLICK_LOOT, 4, drop_id)
    return _rudp_packet(
        channel=RUDP_CHANNEL_INPUT,
        packet_type=RUDP_TYPE_INPUT_COMMAND,
        sequence=120000 + ((cmd_seq - 1) * 1000) + (client_id - FIRST_CLIENT_ID),
        payload=payload,
    )


def _rudp_move_packet(client_id: int, dir_x: int, dir_y: int, cmd_seq: int = 2) -> bytes:
    payload = struct.pack(
        ">IIBBhhH",
        client_id,
        cmd_seq,
        RUDP_INPUT_MOVE,
        6,
        dir_x,
        dir_y,
        0,
    )
    return _rudp_packet(
        channel=RUDP_CHANNEL_INPUT,
        packet_type=RUDP_TYPE_INPUT_COMMAND,
        sequence=105000 + ((cmd_seq - 2) * 1000) + (client_id - FIRST_CLIENT_ID),
        payload=payload,
    )


def _rudp_ack_packet(ack: int, sequence: int) -> bytes:
    return _rudp_packet(
        channel=RUDP_CHANNEL_CONTROL,
        packet_type=RUDP_TYPE_HELLO,
        sequence=sequence,
        payload=b"",
        flags=RUDP_FLAG_ACK_ONLY,
        ack=ack,
    )


def _recv_battle_start(
    udp_socket: socket.socket,
    expected_room_id: int,
    session_id: int,
    timeout_sec: float,
) -> dict[str, int]:
    udp_socket.settimeout(timeout_sec)
    while True:
        data, _ = udp_socket.recvfrom(MAX_RUDP_PACKET_SIZE)
        header, payload = _parse_rudp_packet(data)
        if header["packet_type"] == RUDP_TYPE_BATTLE_START:
            if len(payload) != 20:
                raise ValueError("invalid BattleStart payload")
            room_id, session_a, session_b = struct.unpack(">IQQ", payload)
            if room_id == expected_room_id and session_id in (session_a, session_b):
                return header
            continue
        if header["packet_type"] == RUDP_TYPE_BATTLE_START_ROSTER:
            if len(payload) < 6:
                raise ValueError("invalid BattleStartRoster payload")
            room_id, player_count = struct.unpack(">IH", payload[:6])
            expected_size = 6 + (player_count * 8)
            if len(payload) != expected_size:
                raise ValueError("invalid BattleStartRoster payload size")
            if room_id != expected_room_id:
                continue
            session_ids = [
                struct.unpack(">Q", payload[6 + (index * 8):14 + (index * 8)])[0]
                for index in range(player_count)
            ]
            if any(current_session_id == 0 for current_session_id in session_ids):
                raise ValueError("invalid BattleStartRoster zero session")
            if len(set(session_ids)) != len(session_ids):
                raise ValueError("invalid BattleStartRoster duplicate sessions")
            if session_id in session_ids:
                return header


def _rudp_packet(
    channel: int,
    packet_type: int,
    sequence: int,
    payload: bytes,
    flags: int = 0,
    ack: int = 0,
) -> bytes:
    packet = bytearray(RUDP_HEADER_SIZE + len(payload))
    packet[0:2] = b"LO"
    packet[2] = 1
    packet[3] = flags
    packet[4] = RUDP_HEADER_SIZE
    packet[5] = channel
    struct.pack_into(">H", packet, 6, packet_type)
    struct.pack_into(">I", packet, 8, sequence)
    struct.pack_into(">I", packet, 12, ack)
    struct.pack_into(">I", packet, 16, 0)
    struct.pack_into(">H", packet, 20, len(payload))
    struct.pack_into(">I", packet, 22, 0)
    struct.pack_into(">H", packet, 26, 0)
    packet[RUDP_HEADER_SIZE:] = payload
    struct.pack_into(">I", packet, 22, zlib.crc32(packet) & 0xFFFFFFFF)
    return bytes(packet)


def _parse_rudp_packet(data: bytes) -> tuple[dict[str, int], bytes]:
    if len(data) < RUDP_HEADER_SIZE or len(data) > MAX_RUDP_PACKET_SIZE:
        raise ValueError("invalid RUDP packet size")
    if data[0:2] != b"LO" or data[2] != 1 or data[4] != RUDP_HEADER_SIZE:
        raise ValueError("invalid RUDP packet header")
    if struct.unpack(">H", data[26:28])[0] != 0:
        raise ValueError("invalid RUDP reserved field")

    payload_len = struct.unpack(">H", data[20:22])[0]
    if payload_len != len(data) - RUDP_HEADER_SIZE:
        raise ValueError("invalid RUDP payload length")

    packet = bytearray(data)
    expected_checksum = struct.unpack(">I", packet[22:26])[0]
    packet[22:26] = b"\x00\x00\x00\x00"
    if (zlib.crc32(packet) & 0xFFFFFFFF) != expected_checksum:
        raise ValueError("invalid RUDP checksum")

    header = {
        "flags": data[3],
        "channel": data[5],
        "packet_type": struct.unpack(">H", data[6:8])[0],
        "sequence": struct.unpack(">I", data[8:12])[0],
        "ack": struct.unpack(">I", data[12:16])[0],
        "payload_len": payload_len,
    }
    _validate_rudp_header(header)
    return header, data[RUDP_HEADER_SIZE:]


def _validate_rudp_header(header: dict[str, int]) -> None:
    if header["flags"] & ~(RUDP_FLAG_RELIABLE | RUDP_FLAG_ACK_ONLY):
        raise ValueError("invalid RUDP flags")
    if header["flags"] & RUDP_FLAG_ACK_ONLY and header["payload_len"] != 0:
        raise ValueError("invalid RUDP ack-only payload")
    expected_channels = {
        RUDP_TYPE_HELLO: RUDP_CHANNEL_CONTROL,
        RUDP_TYPE_INPUT_COMMAND: RUDP_CHANNEL_INPUT,
        RUDP_TYPE_BATTLE_START: RUDP_CHANNEL_EVENT,
        RUDP_TYPE_STATE_SNAPSHOT: 0x03,
        RUDP_TYPE_GAME_EVENT: RUDP_CHANNEL_EVENT,
        RUDP_TYPE_META_RESPONSE: RUDP_CHANNEL_CONTROL,
        RUDP_TYPE_BATTLE_START_ROSTER: RUDP_CHANNEL_EVENT,
        RUDP_TYPE_ERROR: RUDP_CHANNEL_CONTROL,
    }
    if header["packet_type"] not in expected_channels:
        raise ValueError("unsupported RUDP packet type")
    if header["channel"] != expected_channels[header["packet_type"]]:
        raise ValueError("invalid RUDP channel")


def _packet(packet_type: int, payload: bytes) -> bytes:
    packet_size = 4 + len(payload)
    if packet_size > MAX_TCP_PACKET_SIZE:
        raise ValueError("packet too large")
    return struct.pack(">HH", packet_size, packet_type) + payload


def _tcp_packet_type_name(packet_type: int | None) -> str | None:
    if packet_type is None:
        return None
    return TCP_PACKET_TYPE_NAMES.get(packet_type, f"unknown_0x{packet_type:04x}")


def _annotate_tcp_receive_error(
    error: ConnectionError,
    expected_packet_type: int,
    ignored_packet_count: int,
    last_packet_type: int | None,
) -> None:
    error.expected_tcp_packet_type = expected_packet_type
    error.tcp_ignored_packet_count = ignored_packet_count
    error.tcp_last_packet_type = last_packet_type


def _record_room_status_ignored_packets(
    result: dict[str, Any],
    ignored_packet_count: int,
    ignored_room_list_snapshot_count: int,
) -> None:
    result["tcp_room_status_ignored_packets"] += ignored_packet_count
    result["tcp_room_status_ignored_room_list_snapshot"] += (
        ignored_room_list_snapshot_count
    )
    result["tcp_room_status_max_ignored_packets_before_response"] = max(
        result["tcp_room_status_max_ignored_packets_before_response"],
        ignored_packet_count,
    )
    result[
        "tcp_room_status_max_ignored_room_list_snapshot_before_response"
    ] = max(
        result[
            "tcp_room_status_max_ignored_room_list_snapshot_before_response"
        ],
        ignored_room_list_snapshot_count,
    )


def _recv_packet(client: socket.socket) -> tuple[int, bytes]:
    try:
        header = _recv_exact(client, 4)
    except ConnectionError as error:
        error.tcp_recv_phase = "header"
        raise
    packet_size, packet_type = struct.unpack(">HH", header)
    if packet_size < 4 or packet_size > MAX_TCP_PACKET_SIZE:
        raise ValueError("invalid TCP packet size")
    try:
        payload = _recv_exact(client, packet_size - 4)
    except ConnectionError as error:
        error.tcp_recv_phase = "payload"
        error.tcp_packet_type = packet_type
        raise
    return packet_type, payload


def _recv_exact(client: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = client.recv(size - len(data))
        if not chunk:
            raise ConnectionError("socket closed")
        data.extend(chunk)
    return bytes(data)


def _write_json(data: dict[str, Any], output_path: str | None) -> None:
    content = json.dumps(data, indent=2, sort_keys=True) + "\n"
    if output_path:
        Path(output_path).write_text(content, encoding="utf-8")
        return
    print(content, end="")


if __name__ == "__main__":
    raise SystemExit(main())
