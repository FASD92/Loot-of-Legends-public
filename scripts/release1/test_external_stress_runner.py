import json
import socket
import struct
import tempfile
import threading
import unittest
import zlib
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from typing import Optional

import scripts.release1.external_stress_runner as runner
from scripts.release1.external_stress_runner import (
    main,
    run_external_tcp_room_fill,
)


TYPE_WELCOME = 0x0001
TYPE_CLIENT_LIST_SNAPSHOT = 0x0002
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
TYPE_ROOM_LIST_SNAPSHOT = 0x0107
RUDP_CHANNEL_CONTROL = 0x01
RUDP_CHANNEL_INPUT = 0x02
RUDP_CHANNEL_EVENT = 0x04
RUDP_FLAG_RELIABLE = 0x01
RUDP_FLAG_ACK_ONLY = 0x02
RUDP_TYPE_HELLO = 0x1001
RUDP_TYPE_INPUT_COMMAND = 0x1002
RUDP_TYPE_BATTLE_START = 0x1003
RUDP_TYPE_GAME_EVENT = 0x1005
RUDP_TYPE_BATTLE_START_ROSTER = 0x1007
RUDP_INPUT_READY = 0x01
RUDP_INPUT_CLICK_LOOT = 0x03
RUDP_INPUT_MOVE = 0x04
RUDP_INPUT_ATTACK = 0x05


class ExternalStressRunnerTests(unittest.TestCase):
    def test_parse_rudp_loot_resolved_payload_skips_stale_drop(self):
        expected_drop = {
            "room_id": 10,
            "drop_id": 222,
            "item_id": 1001,
            "quantity": 1,
        }
        stale_payload = struct.pack(
            ">HHIIQIH",
            runner.RUDP_GAME_EVENT_LOOT_RESOLVED,
            runner.RUDP_LOOT_RESOLVED_BODY_SIZE,
            10,
            111,
            999,
            1001,
            1,
        )

        self.assertIsNone(
            runner._parse_rudp_loot_resolved_payload(stale_payload, expected_drop)
        )

    def test_rudp_loot_fanout_timeout_records_drop_context(self):
        class TimeoutUdpSocket:
            def __init__(self):
                self.timeout = None

            def settimeout(self, timeout):
                self.timeout = timeout

            def recvfrom(self, _max_size):
                raise socket.timeout("timed out")

        result = {
            "rudp_loot_timeout_context": None,
            "loot_resolved_events": [],
            "rudp_loot_resolved_received": 0,
        }
        expected_drop = {
            "room_id": 700,
            "drop_id": 2705,
            "item_id": 101,
            "quantity": 1,
        }

        with self.assertRaises(socket.timeout):
            runner._recv_rudp_loot_resolved_fanout(
                ("127.0.0.1", 7777),
                [{"client_id": 100123, "udp": TimeoutUdpSocket()}],
                expected_drop,
                result,
                0.01,
                drop_index=5,
                drop_count=9,
            )

        self.assertEqual(
            {
                "room_id": 700,
                "drop_id": 2705,
                "item_id": 101,
                "quantity": 1,
                "drop_index": 5,
                "drop_count": 9,
                "expected_fanout": 1,
                "received_fanout": 0,
                "timed_out_client_index": 0,
                "timed_out_client_id": 100123,
            },
            result["rudp_loot_timeout_context"],
        )

    def test_runner_fills_rooms_against_external_tcp_server(self):
        with FakeRoomFillServer(participants=4, room_size=2) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=4,
                room_size=2,
                timeout_sec=2.0,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"])
        self.assertEqual("", result["reason"])
        self.assertEqual(4, result["connected"])
        self.assertEqual(4, result["welcome_received"])
        self.assertEqual(2, result["rooms_created"])
        self.assertEqual(2, result["rooms_filled"])
        self.assertEqual(
            [
                TYPE_CREATE_ROOM_REQUEST,
                TYPE_JOIN_ROOM_REQUEST,
                TYPE_CREATE_ROOM_REQUEST,
                TYPE_JOIN_ROOM_REQUEST,
            ],
            server.observed_request_types,
        )

    def test_runner_counts_room_status_ignored_room_list_snapshots(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            room_list_snapshots_before_room_status=2,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"])
        self.assertEqual(6, result["tcp_room_status_ignored_packets"])
        self.assertEqual(4, result["tcp_room_status_ignored_room_list_snapshot"])
        self.assertEqual(
            3,
            result["tcp_room_status_max_ignored_packets_before_response"],
        )
        self.assertEqual(
            2,
            result["tcp_room_status_max_ignored_room_list_snapshot_before_response"],
        )

    def test_runner_rejects_partial_room_before_socket_work(self):
        with self.assertRaisesRegex(ValueError, "participants must be divisible"):
            run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=1,
                participants=3,
                room_size=2,
                timeout_sec=0.1,
            )

    def test_runner_authenticates_before_waiting_for_welcome(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            require_auth=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                auth_token_prefix="dev-session:",
            )

        server.assert_no_error()
        self.assertTrue(result["valid"])
        self.assertEqual(
            ["dev-session:P000000", "dev-session:P000001"],
            server.observed_auth_tokens,
        )

    def test_cli_prints_json_result(self):
        with FakeRoomFillServer(participants=2, room_size=2) as server:
            stdout = StringIO()
            with redirect_stdout(stdout):
                status = main(
                    [
                        "--host",
                        "127.0.0.1",
                        "--tcp-port",
                        str(server.port),
                        "--participants",
                        "2",
                        "--room-size",
                        "2",
                        "--timeout-sec",
                        "2",
                    ]
                )

        server.assert_no_error()
        self.assertEqual(0, status)
        payload = json.loads(stdout.getvalue())
        self.assertTrue(payload["valid"])
        self.assertEqual("tcp_room_fill", payload["workload"])
        self.assertEqual(2, payload["connected"])

    def test_runner_binds_rudp_ready_and_ack_battle_start(self):
        with FakeRoomFillServer(participants=2, room_size=2, enable_udp=True) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"])
        self.assertEqual(2, result["rudp_hello_sent"])
        self.assertEqual(2, result["rudp_ready_sent"])
        self.assertEqual(2, result["battle_start_received"])
        self.assertEqual(2, result["rudp_ack_sent"])
        self.assertEqual(
            [
                RUDP_TYPE_HELLO,
                RUDP_TYPE_HELLO,
                RUDP_TYPE_INPUT_COMMAND,
                RUDP_TYPE_INPUT_COMMAND,
                RUDP_TYPE_HELLO,
                RUDP_TYPE_HELLO,
            ],
            server.observed_rudp_packet_types,
        )

    def test_runner_uses_configured_client_id_base_for_rudp_packets(self):
        with FakeRoomFillServer(participants=2, room_size=2, enable_udp=True) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                client_id_base=200000,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"])
        self.assertEqual(200000, result["client_id_base"])
        self.assertEqual(
            [200000, 200001, 200000, 200001],
            server.observed_rudp_client_ids[:4],
        )

    def test_runner_waits_between_rudp_hello_and_ready_when_configured(self):
        sleep_calls = []
        original_sleep = runner.time.sleep

        def fake_sleep(seconds):
            sleep_calls.append(seconds)

        runner.time.sleep = fake_sleep
        try:
            with FakeRoomFillServer(participants=2, room_size=2, enable_udp=True) as server:
                result = run_external_tcp_room_fill(
                    host="127.0.0.1",
                    tcp_port=server.port,
                    participants=2,
                    room_size=2,
                    timeout_sec=2.0,
                    udp_port=server.port,
                    rudp_handshake_settle_sec=0.25,
                )
        finally:
            runner.time.sleep = original_sleep

        server.assert_no_error()
        self.assertTrue(result["valid"])
        self.assertEqual(0.25, result["rudp_handshake_settle_sec"])
        self.assertEqual([0.25], sleep_calls)

    def test_runner_can_send_tcp_ready_and_host_start_for_rudp_smoke(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            enable_udp=True,
            require_tcp_start=True,
            send_health_snapshot_after_attack=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(2, result["tcp_ready_sent"])
        self.assertEqual(1, result["tcp_host_start_sent"])
        self.assertEqual(
            [
                TYPE_CREATE_ROOM_REQUEST,
                TYPE_JOIN_ROOM_REQUEST,
                TYPE_READY_ROOM_REQUEST,
                TYPE_READY_ROOM_REQUEST,
                TYPE_HOST_START_BATTLE_REQUEST,
            ],
            server.observed_request_types,
        )

    def test_runner_waits_for_ready_responses_before_host_start(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            enable_udp=True,
            require_tcp_start=True,
            enforce_ready_response_before_host_start=True,
            send_health_snapshot_after_attack=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(2, result["tcp_ready_responses"])
        self.assertEqual(1, result["tcp_host_start_responses"])

    def test_runner_receives_tcp_battle_start_roster_for_ten_player_room(self):
        with FakeRoomFillServer(
            participants=10,
            room_size=10,
            require_tcp_start=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=10,
                room_size=10,
                timeout_sec=2.0,
                start_battle=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(10, result["tcp_ready_responses"])
        self.assertEqual(1, result["tcp_host_start_responses"])
        self.assertEqual(0, result["tcp_battle_start_received"])
        self.assertEqual(10, result["tcp_battle_start_roster_received"])

    def test_runner_completes_tcp_arena_entry_after_battle_start(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            require_tcp_start=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                start_battle=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(2, result["tcp_battle_load_entry_received"])
        self.assertEqual(2, result["tcp_arena_load_complete_sent"])
        self.assertEqual(2, result["tcp_arena_gameplay_start_received"])

    def test_runner_captures_monster_spawn_after_arena_start(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            require_tcp_start=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                start_battle=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(2, result["tcp_monster_spawn_received"])
        self.assertEqual(
            [
                {
                    "room_id": 700,
                    "monster_id": 1700,
                    "monster_type_id": 1,
                    "max_hp": 30,
                }
            ],
            result["spawned_monsters"],
        )

    def test_runner_sends_rudp_attack_and_receives_tcp_health_snapshot(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            enable_udp=True,
            require_tcp_start=True,
            send_health_snapshot_after_attack=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(2, result["rudp_attack_sent"])
        self.assertEqual(2, result["rudp_attack_target_sent"])
        self.assertEqual(0, result["rudp_click_loot_target_sent"])
        self.assertEqual(2, result["tcp_monster_health_snapshot_received"])
        self.assertEqual(
            [
                {
                    "room_id": 700,
                    "monster_id": 1700,
                    "current_hp": 5,
                    "max_hp": 30,
                }
            ],
            result["monster_health_snapshots"],
        )

    def test_runner_sends_rudp_move_after_battle_start_ready(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            enable_udp=True,
            require_tcp_start=True,
            expect_rudp_move_after_ready=True,
            send_health_snapshot_after_attack=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(2, result["rudp_move_sent"])
        self.assertEqual(2, result["rudp_move_target_sent"])

    def test_runner_sends_sustained_rudp_move_workload(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            enable_udp=True,
            require_tcp_start=True,
            expect_rudp_move_after_ready=True,
            expected_rudp_move_packets_per_player=3,
            send_health_snapshot_after_attack=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
                move_duration_sec=0.03,
                move_rate_hz=100.0,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(6, result["rudp_move_sent"])
        self.assertEqual(6, result["rudp_move_target_sent"])
        self.assertEqual(2, result["rudp_attack_sent"])

    def test_runner_repeats_rudp_attack_until_drop_then_clicks_loot(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            enable_udp=True,
            require_tcp_start=True,
            expect_rudp_move_after_ready=True,
            send_drop_after_attack_death=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
                loot_smoke=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(2, result["rudp_move_target_sent"])
        self.assertEqual(8, result["rudp_attack_sent"])
        self.assertEqual(8, result["rudp_attack_target_sent"])
        self.assertEqual(2, result["tcp_monster_death_received"])
        self.assertEqual(2, result["tcp_drop_list_snapshot_received"])
        self.assertEqual(
            [
                {
                    "room_id": 700,
                    "drop_id": 2700,
                    "item_id": 101,
                    "quantity": 1,
                }
            ],
            result["drops_observed"],
        )
        self.assertEqual(2, result["rudp_click_loot_sent"])
        self.assertEqual(2, result["rudp_click_loot_target_sent"])
        self.assertEqual(2, result["rudp_loot_resolved_received"])
        self.assertEqual(
            [
                {
                    "room_id": 700,
                    "drop_id": 2700,
                    "winner_session_id": 1000,
                    "item_id": 101,
                    "quantity": 1,
                }
            ],
            result["loot_resolved_events"],
        )

    def test_runner_sends_sustained_rudp_attack_workload_before_loot(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            enable_udp=True,
            require_tcp_start=True,
            expect_rudp_move_after_ready=True,
            expected_rudp_attack_packets_per_player=4,
            send_drop_after_attack_death=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
                loot_smoke=True,
                attack_duration_sec=0.04,
                attack_rate_hz=100.0,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(8, result["rudp_attack_target_sent"])
        self.assertEqual(8, result["rudp_attack_sent"])
        self.assertEqual(2, result["rudp_click_loot_sent"])

    def test_runner_runs_ten_player_rudp_loot_smoke(self):
        with FakeRoomFillServer(
            participants=10,
            room_size=10,
            enable_udp=True,
            require_tcp_start=True,
            expect_rudp_move_after_ready=True,
            send_drop_after_attack_death=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=10,
                room_size=10,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
                loot_smoke=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(10, result["battle_start_received"])
        self.assertEqual(10, result["rudp_move_target_sent"])
        self.assertEqual(10, result["rudp_move_sent"])
        self.assertEqual(40, result["rudp_attack_target_sent"])
        self.assertEqual(40, result["rudp_attack_sent"])
        self.assertEqual(9, len(result["drops_observed"]))
        self.assertEqual(18, result["rudp_click_loot_target_sent"])
        self.assertEqual(18, result["rudp_click_loot_sent"])
        self.assertEqual(90, result["rudp_loot_resolved_received"])

    def test_runner_keeps_single_drop_loser_clicks_out_of_server_accept_target(self):
        with FakeRoomFillServer(
            participants=10,
            room_size=10,
            enable_udp=True,
            require_tcp_start=True,
            expect_rudp_move_after_ready=True,
            send_drop_after_attack_death=True,
            single_drop_after_attack_death=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=10,
                room_size=10,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
                loot_smoke=True,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(1, len(result["drops_observed"]))
        self.assertEqual(10, result["rudp_click_loot_target_sent"])
        self.assertEqual(10, result["rudp_click_loot_sent"])
        self.assertEqual(1, result["rudp_click_loot_server_accept_target_sent"])
        self.assertEqual(10, result["rudp_loot_resolved_received"])

    def test_runner_repeats_battle_cycles_with_fresh_rooms(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            battle_cycles=2,
            enable_udp=True,
            require_tcp_start=True,
            expect_rudp_move_after_ready=True,
            send_drop_after_attack_death=True,
            send_battle_result_after_loot=True,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
                loot_smoke=True,
                battle_cycles=2,
            )

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(2, result["battle_cycles"])
        self.assertEqual(2, result["rooms_created"])
        self.assertEqual(2, result["rooms_filled"])
        self.assertEqual(4, result["rudp_click_loot_sent"])
        self.assertEqual(4, result["tcp_battle_final_ranking_received"])
        self.assertEqual(4, result["tcp_lobby_return_received"])

    def test_runner_records_failure_stage_when_socket_closes_between_cycles(self):
        with FakeRoomFillServer(
            participants=2,
            room_size=2,
            battle_cycles=2,
            enable_udp=True,
            require_tcp_start=True,
            expect_rudp_move_after_ready=True,
            send_drop_after_attack_death=True,
            send_battle_result_after_loot=True,
            close_clients_after_cycle=0,
        ) as server:
            result = run_external_tcp_room_fill(
                host="127.0.0.1",
                tcp_port=server.port,
                participants=2,
                room_size=2,
                timeout_sec=2.0,
                udp_port=server.port,
                start_battle=True,
                loot_smoke=True,
                battle_cycles=2,
            )

        server.assert_no_error()
        self.assertFalse(result["valid"], result)
        self.assertEqual("socket closed", result["reason"])
        self.assertEqual("create_room", result["failure_stage"])
        self.assertEqual(1, result["failure_cycle"])
        self.assertEqual(0, result["failure_room_first"])
        self.assertIsNone(result["failure_join_offset"])
        self.assertEqual("ConnectionError", result["failure_error_type"])
        self.assertIsNone(result["failure_error_errno"])
        self.assertEqual(0, result["failure_client_index"])
        self.assertEqual(100000, result["failure_client_id"])
        self.assertEqual(1000, result["failure_session_id"])
        self.assertEqual(2, result["failure_connected_clients"])
        self.assertEqual(
            TYPE_CREATE_ROOM_RESPONSE,
            result["failure_expected_tcp_packet_type"],
        )
        self.assertEqual(
            "create_room_response",
            result["failure_expected_tcp_packet_name"],
        )
        self.assertEqual("header", result["failure_tcp_recv_phase"])
        self.assertIsNone(result["failure_tcp_packet_type"])
        self.assertIsNone(result["failure_tcp_packet_name"])
        self.assertEqual(0, result["failure_tcp_ignored_packet_count"])
        self.assertIsNone(result["failure_tcp_last_packet_type"])
        self.assertIsNone(result["failure_tcp_last_packet_name"])

    def test_runner_binds_rudp_before_tcp_start_battle(self):
        events: list[str] = []
        original_rudp_hellos = runner._send_rudp_hellos
        original_rudp_ready = runner._send_rudp_ready_and_ack_battle_start
        original_rudp_attack = runner._send_rudp_attack_and_recv_health_snapshot
        original_tcp_start_battle = runner._send_tcp_start_battle

        def fake_rudp_hellos(
            host,
            udp_port,
            room_clients,
            result,
        ):
            events.append("rudp_bind")
            result["rudp_hello_sent"] += len(room_clients)

        def fake_rudp_ready(
            host,
            udp_port,
            room_id,
            room_clients,
            timeout_sec,
            result,
            ready_cmd_seq=1,
        ):
            result["rudp_ready_sent"] += len(room_clients)
            result["battle_start_received"] += len(room_clients)
            result["rudp_ack_sent"] += len(room_clients)

        def fake_tcp_start_battle(room_clients, result, timeout_sec):
            events.append("tcp_start")
            result["tcp_ready_sent"] += len(room_clients)
            result["tcp_ready_responses"] += len(room_clients)
            result["tcp_host_start_sent"] += 1
            result["tcp_host_start_responses"] += 1
            result["tcp_battle_start_received"] += len(room_clients)
            result["tcp_battle_load_entry_received"] += len(room_clients)
            result["tcp_arena_load_complete_sent"] += len(room_clients)
            result["tcp_arena_gameplay_start_received"] += len(room_clients)
            result["tcp_monster_spawn_received"] += len(room_clients)
            result["spawned_monsters"].append(
                {
                    "room_id": 700,
                    "monster_id": 1700,
                    "monster_type_id": 1,
                    "max_hp": 30,
                }
            )
            return {
                "room_id": 700,
                "monster_id": 1700,
                "monster_type_id": 1,
                "max_hp": 30,
            }

        def fake_rudp_attack(
            host,
            udp_port,
            room_clients,
            spawned_monster,
            timeout_sec,
            result,
            cmd_seq,
        ):
            result["rudp_attack_sent"] += len(room_clients)
            result["tcp_monster_health_snapshot_received"] += len(room_clients)
            result["monster_health_snapshots"].append(
                {
                    "room_id": spawned_monster["room_id"],
                    "monster_id": spawned_monster["monster_id"],
                    "current_hp": 5,
                    "max_hp": spawned_monster["max_hp"],
                }
            )

        runner._send_rudp_hellos = fake_rudp_hellos
        runner._send_rudp_ready_and_ack_battle_start = fake_rudp_ready
        runner._send_rudp_attack_and_recv_health_snapshot = fake_rudp_attack
        runner._send_tcp_start_battle = fake_tcp_start_battle
        try:
            with FakeRoomFillServer(participants=2, room_size=2) as server:
                result = runner.run_external_tcp_room_fill(
                    host="127.0.0.1",
                    tcp_port=server.port,
                    participants=2,
                    room_size=2,
                    timeout_sec=2.0,
                    udp_port=server.port,
                    start_battle=True,
                )
        finally:
            runner._send_rudp_hellos = original_rudp_hellos
            runner._send_rudp_ready_and_ack_battle_start = original_rudp_ready
            runner._send_rudp_attack_and_recv_health_snapshot = original_rudp_attack
            runner._send_tcp_start_battle = original_tcp_start_battle

        server.assert_no_error()
        self.assertTrue(result["valid"], result)
        self.assertEqual(["rudp_bind", "tcp_start"], events)

    def test_cli_writes_json_output_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "runner.json"
            with FakeRoomFillServer(participants=2, room_size=2) as server:
                status = main(
                    [
                        "--host",
                        "127.0.0.1",
                        "--tcp-port",
                        str(server.port),
                        "--participants",
                        "2",
                        "--room-size",
                        "2",
                        "--timeout-sec",
                        "2",
                        "--output",
                        str(output),
                    ]
                )

            server.assert_no_error()
            self.assertEqual(0, status)
            payload = json.loads(output.read_text(encoding="utf-8"))
            self.assertTrue(payload["valid"])
            self.assertEqual(1, payload["rooms_filled"])


class FakeRoomFillServer:
    def __init__(
        self,
        participants: int,
        room_size: int,
        enable_udp: bool = False,
        require_auth: bool = False,
        require_tcp_start: bool = False,
        enforce_ready_response_before_host_start: bool = False,
        expect_rudp_move_after_ready: bool = False,
        expected_rudp_move_packets_per_player: int = 1,
        expected_rudp_attack_packets_per_player: int = 4,
        send_health_snapshot_after_attack: bool = False,
        send_drop_after_attack_death: bool = False,
        single_drop_after_attack_death: bool = False,
        battle_cycles: int = 1,
        send_battle_result_after_loot: bool = False,
        close_clients_after_cycle: Optional[int] = None,
        room_list_snapshots_before_room_status: int = 0,
        port: int = 0,
    ):
        self.participants = participants
        self.room_size = room_size
        self.enable_udp = enable_udp
        self.require_auth = require_auth
        self.require_tcp_start = require_tcp_start
        self.enforce_ready_response_before_host_start = (
            enforce_ready_response_before_host_start
        )
        self.expect_rudp_move_after_ready = expect_rudp_move_after_ready
        self.expected_rudp_move_packets_per_player = expected_rudp_move_packets_per_player
        self.expected_rudp_attack_packets_per_player = expected_rudp_attack_packets_per_player
        self.send_health_snapshot_after_attack = send_health_snapshot_after_attack
        self.send_drop_after_attack_death = send_drop_after_attack_death
        self.single_drop_after_attack_death = single_drop_after_attack_death
        self.battle_cycles = battle_cycles
        self.send_battle_result_after_loot = send_battle_result_after_loot
        self.close_clients_after_cycle = close_clients_after_cycle
        self.room_list_snapshots_before_room_status = room_list_snapshots_before_room_status
        self.observed_request_types: list[int] = []
        self.observed_rudp_packet_types: list[int] = []
        self.observed_rudp_client_ids: list[int] = []
        self.observed_auth_tokens: list[str] = []
        self.error: BaseException | None = None
        self._udp_endpoints_by_room_cycle: dict[tuple[int, int], list[tuple[str, int]]] = {}
        self._clients: list[socket.socket] = []
        self._ready = threading.Event()
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", port))
        self._listener.listen(participants)
        self.port = self._listener.getsockname()[1]
        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if self.enable_udp:
            self._udp.settimeout(2.0)
            self._udp.bind(("127.0.0.1", self.port))
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self):
        self._thread.start()
        self._ready.wait(timeout=2.0)
        return self

    def __exit__(self, exc_type, exc, tb):
        self._thread.join(timeout=2.0)
        self._listener.close()
        self._udp.close()
        for client in self._clients:
            client.close()

    def assert_no_error(self):
        if self.error is not None:
            raise AssertionError(self.error)

    def _run(self):
        try:
            self._ready.set()
            for index in range(self.participants):
                client, _ = self._listener.accept()
                client.settimeout(2.0)
                self._clients.append(client)
                if self.require_auth:
                    auth_packet = _recv_packet(client)
                    self.observed_auth_tokens.append(_auth_token(auth_packet))
                _send_packet(client, TYPE_WELCOME, struct.pack(">Q", 1000 + index))
                _send_packet(client, TYPE_CLIENT_LIST_SNAPSHOT, struct.pack(">H", 0))

            room_id = 700
            for cycle_index in range(self.battle_cycles):
                for first in range(0, self.participants, self.room_size):
                    create_packet = _recv_packet(self._clients[first])
                    self.observed_request_types.append(_packet_type(create_packet))
                    _send_empty_room_list_snapshots(
                        self._clients[first],
                        self.room_list_snapshots_before_room_status,
                    )
                    _send_room_status(
                        self._clients[first],
                        TYPE_CREATE_ROOM_RESPONSE,
                        room_id,
                        1,
                    )

                    for offset in range(1, self.room_size):
                        client = self._clients[first + offset]
                        join_packet = _recv_packet(client)
                        self.observed_request_types.append(_packet_type(join_packet))
                        _send_empty_room_list_snapshots(
                            client,
                            self.room_list_snapshots_before_room_status,
                        )
                        _send_packet(
                            client,
                            TYPE_JOIN_ROOM_RESPONSE,
                            struct.pack(">IH", room_id, offset + 1),
                        )
                    if self.require_tcp_start:
                        for offset in range(self.room_size):
                            ready_packet = _recv_packet(self._clients[first + offset])
                            self.observed_request_types.append(_packet_type(ready_packet))
                            if (
                                self.enforce_ready_response_before_host_start and
                                _tcp_packet_waiting(self._clients[first])
                            ):
                                raise AssertionError("HostStart arrived before ReadyResponse")
                            _send_packet(
                                self._clients[first + offset],
                                TYPE_READY_ROOM_RESPONSE,
                                struct.pack(">IHH", room_id, offset + 1, self.room_size),
                            )
                        host_start_packet = _recv_packet(self._clients[first])
                        self.observed_request_types.append(_packet_type(host_start_packet))
                        _send_packet(
                            self._clients[first],
                            TYPE_HOST_START_BATTLE_RESPONSE,
                            struct.pack(">I", room_id),
                        )
                        battle_instance_id = 900000 + room_id
                        self._send_battle_start(room_id, first)
                        self._send_battle_load_entry(room_id, battle_instance_id, first)
                        self._recv_arena_load_complete(room_id, battle_instance_id, first)
                        self._send_arena_gameplay_start(room_id, battle_instance_id, first)
                        self._send_monster_spawn(room_id, first)
                    if self.enable_udp:
                        self._run_udp_ready_exchange(room_id, first, cycle_index)
                    room_id += 1
                if self.close_clients_after_cycle == cycle_index:
                    for client in self._clients:
                        client.close()
                    return
        except BaseException as error:
            self.error = error

    def _send_battle_start(self, room_id: int, first: int) -> None:
        player_session_ids = [
            1000 + first + offset
            for offset in range(self.room_size)
        ]
        if self.room_size == 2:
            packet_type = TYPE_BATTLE_START
            payload = struct.pack(
                ">IQQ",
                room_id,
                player_session_ids[0],
                player_session_ids[1],
            )
        else:
            packet_type = TYPE_BATTLE_START_ROSTER
            payload = struct.pack(">IH", room_id, self.room_size) + b"".join(
                struct.pack(">Q", session_id)
                for session_id in player_session_ids
            )

        for offset in range(self.room_size):
            _send_packet(self._clients[first + offset], packet_type, payload)

    def _send_battle_load_entry(
        self,
        room_id: int,
        battle_instance_id: int,
        first: int,
    ) -> None:
        player_session_ids = [
            1000 + first + offset
            for offset in range(self.room_size)
        ]
        payload = (
            struct.pack(">IQH", room_id, battle_instance_id, self.room_size) +
            b"".join(struct.pack(">Q", session_id) for session_id in player_session_ids)
        )
        for offset in range(self.room_size):
            _send_packet(self._clients[first + offset], TYPE_BATTLE_LOAD_ENTRY, payload)

    def _recv_arena_load_complete(
        self,
        room_id: int,
        battle_instance_id: int,
        first: int,
    ) -> None:
        for offset in range(self.room_size):
            packet = _recv_packet(self._clients[first + offset])
            if _packet_type(packet) != TYPE_ARENA_LOAD_COMPLETE:
                raise AssertionError("expected ArenaLoadComplete packet")
            loaded_room_id, loaded_battle_instance_id = struct.unpack(">IQ", packet[4:16])
            if loaded_room_id != room_id or loaded_battle_instance_id != battle_instance_id:
                raise AssertionError("invalid ArenaLoadComplete state")

    def _send_arena_gameplay_start(
        self,
        room_id: int,
        battle_instance_id: int,
        first: int,
    ) -> None:
        payload = struct.pack(">IQ", room_id, battle_instance_id)
        for offset in range(self.room_size):
            _send_packet(self._clients[first + offset], TYPE_ARENA_GAMEPLAY_START, payload)

    def _send_monster_spawn(self, room_id: int, first: int) -> None:
        payload = struct.pack(">IIIH", room_id, 1000 + room_id, 1, 30)
        for offset in range(self.room_size):
            _send_packet(self._clients[first + offset], TYPE_MONSTER_SPAWN, payload)

    def _run_udp_ready_exchange(
        self,
        room_id: int,
        first: int,
        cycle_index: int,
    ) -> None:
        ready_endpoints: list[tuple[str, int]] = []
        ready_cmd_seq = self._first_ready_cmd_seq_for_cycle(cycle_index)
        while len(ready_endpoints) < self.room_size:
            packet, endpoint = self._udp.recvfrom(2048)
            packet_type = _rudp_packet_type(packet)
            self.observed_rudp_packet_types.append(packet_type)
            client_id = _rudp_client_id(packet)
            if client_id is not None:
                self.observed_rudp_client_ids.append(client_id)
            if packet_type == RUDP_TYPE_INPUT_COMMAND:
                payload = _rudp_payload(packet)
                if len(payload) != 10:
                    raise AssertionError("invalid RUDP Ready payload size")
                player_id, cmd_seq, op, arg_len = struct.unpack(">IIBB", payload)
                if (
                    player_id == 0 or
                    cmd_seq != ready_cmd_seq or
                    op != RUDP_INPUT_READY or
                    arg_len != 0
                ):
                    raise AssertionError("invalid RUDP Ready payload")
                ready_endpoints.append(endpoint)

        roster_payload = struct.pack(">IH", room_id, self.room_size) + b"".join(
            struct.pack(">Q", 1000 + first + offset)
            for offset in range(self.room_size)
        )
        battle_start_payload = struct.pack(">IQQ", room_id, 1000 + first, 1000 + first + 1)
        for offset, endpoint in enumerate(ready_endpoints):
            self._udp.sendto(
                _rudp_packet(
                    channel=RUDP_CHANNEL_EVENT,
                    packet_type=RUDP_TYPE_BATTLE_START_ROSTER,
                    sequence=8000 + offset,
                    payload=roster_payload,
                    flags=RUDP_FLAG_RELIABLE,
                ),
                endpoint,
            )
            self._udp.sendto(
                _rudp_packet(
                    channel=RUDP_CHANNEL_EVENT,
                    packet_type=RUDP_TYPE_BATTLE_START,
                    sequence=9000 + offset,
                    payload=battle_start_payload,
                    flags=RUDP_FLAG_RELIABLE,
                ),
                endpoint,
            )
        self._udp_endpoints_by_room_cycle[(cycle_index, first)] = ready_endpoints

        ack_count = 0
        while ack_count < self.room_size:
            packet, _ = self._udp.recvfrom(2048)
            self.observed_rudp_packet_types.append(_rudp_packet_type(packet))
            client_id = _rudp_client_id(packet)
            if client_id is not None:
                self.observed_rudp_client_ids.append(client_id)
            ack_count += 1

        if (
            self.expect_rudp_move_after_ready or
            self.send_health_snapshot_after_attack or
            self.send_drop_after_attack_death
        ):
            self._recv_rudp_moves(room_id, first, cycle_index)
        if self.send_health_snapshot_after_attack:
            self._recv_rudp_attacks_and_send_health_snapshot(room_id, first, cycle_index)
        if self.send_drop_after_attack_death:
            self._recv_rudp_attacks_and_send_death_and_drop(room_id, first, cycle_index)
            self._recv_rudp_click_loots_and_send_resolved(room_id, first, cycle_index)

    def _first_ready_cmd_seq_for_cycle(self, cycle_index: int) -> int:
        loot_command_slots = max(1, self.room_size - 1) if self.send_drop_after_attack_death else 0
        command_count = (
            1 +
            self.expected_rudp_move_packets_per_player +
            self.expected_rudp_attack_packets_per_player +
            loot_command_slots
        )
        return 1 + (cycle_index * command_count)

    def _recv_rudp_moves(self, room_id: int, first: int, cycle_index: int) -> None:
        expected_moves = self.room_size * self.expected_rudp_move_packets_per_player
        first_move_cmd_seq = self._first_ready_cmd_seq_for_cycle(cycle_index) + 1
        moves_by_player: dict[int, int] = {}
        while sum(moves_by_player.values()) < expected_moves:
            packet, _ = self._udp.recvfrom(2048)
            self.observed_rudp_packet_types.append(_rudp_packet_type(packet))
            client_id = _rudp_client_id(packet)
            if client_id is not None:
                self.observed_rudp_client_ids.append(client_id)
            payload = _rudp_payload(packet)
            if _rudp_packet_type(packet) != RUDP_TYPE_INPUT_COMMAND:
                continue
            if len(payload) != 16:
                raise AssertionError("invalid RUDP Move payload size")
            player_id, cmd_seq, op, arg_len, dir_x, dir_y, flags = struct.unpack(
                ">IIBBhhH",
                payload,
            )
            current_count = moves_by_player.get(player_id, 0)
            if (
                player_id == 0 or
                cmd_seq != first_move_cmd_seq + current_count or
                op != RUDP_INPUT_MOVE or
                arg_len != 6
            ):
                raise AssertionError("invalid RUDP Move payload")
            if dir_x == 0 and dir_y == 0:
                raise AssertionError("invalid RUDP Move direction")
            if flags != 0:
                raise AssertionError("invalid RUDP Move flags")
            moves_by_player[player_id] = current_count + 1

    def _recv_rudp_attacks_and_send_health_snapshot(
        self,
        room_id: int,
        first: int,
        cycle_index: int,
    ) -> None:
        monster_id = 1000 + room_id
        expected_cmd_seq = (
            self._first_ready_cmd_seq_for_cycle(cycle_index) +
            1 +
            self.expected_rudp_move_packets_per_player
        )
        attacks = 0
        while attacks < self.room_size:
            packet, _ = self._udp.recvfrom(2048)
            self.observed_rudp_packet_types.append(_rudp_packet_type(packet))
            client_id = _rudp_client_id(packet)
            if client_id is not None:
                self.observed_rudp_client_ids.append(client_id)
            payload = _rudp_payload(packet)
            if _rudp_packet_type(packet) != RUDP_TYPE_INPUT_COMMAND:
                continue
            if len(payload) != 14:
                raise AssertionError("invalid RUDP Attack payload size")
            player_id, cmd_seq, op, arg_len, target_monster_id = struct.unpack(
                ">IIBBI",
                payload,
            )
            if (
                player_id == 0 or
                cmd_seq != expected_cmd_seq or
                op != RUDP_INPUT_ATTACK or
                arg_len != 4
            ):
                raise AssertionError("invalid RUDP Attack payload")
            if target_monster_id != monster_id:
                raise AssertionError("invalid RUDP Attack target")
            attacks += 1

        payload = struct.pack(">IIHH", room_id, monster_id, 5, 30)
        for offset in range(self.room_size):
            _send_packet(
                self._clients[first + offset],
                TYPE_MONSTER_HEALTH_SNAPSHOT,
                payload,
            )

    def _recv_rudp_attacks_and_send_death_and_drop(
        self,
        room_id: int,
        first: int,
        cycle_index: int,
    ) -> None:
        monster_id = 1000 + room_id
        first_attack_cmd_seq = (
            self._first_ready_cmd_seq_for_cycle(cycle_index) +
            1 +
            self.expected_rudp_move_packets_per_player
        )
        attack_counts_by_player: dict[int, int] = {}
        expected_attacks = self.room_size * self.expected_rudp_attack_packets_per_player
        while sum(attack_counts_by_player.values()) < expected_attacks:
            packet, _ = self._udp.recvfrom(2048)
            self.observed_rudp_packet_types.append(_rudp_packet_type(packet))
            client_id = _rudp_client_id(packet)
            if client_id is not None:
                self.observed_rudp_client_ids.append(client_id)
            payload = _rudp_payload(packet)
            if _rudp_packet_type(packet) != RUDP_TYPE_INPUT_COMMAND:
                continue
            if len(payload) != 14:
                raise AssertionError("invalid RUDP Attack payload size")
            player_id, cmd_seq, op, arg_len, target_monster_id = struct.unpack(
                ">IIBBI",
                payload,
            )
            current_count = attack_counts_by_player.get(player_id, 0)
            if (
                player_id == 0 or
                cmd_seq != first_attack_cmd_seq + current_count or
                op != RUDP_INPUT_ATTACK or
                arg_len != 4
            ):
                raise AssertionError("invalid RUDP Attack payload")
            if target_monster_id != monster_id:
                raise AssertionError("invalid RUDP Attack target")
            attack_counts_by_player[player_id] = current_count + 1

        death_payload = struct.pack(">II", room_id, monster_id)
        drop_count = 1 if self.single_drop_after_attack_death else max(1, self.room_size - 1)
        drop_entries = bytearray()
        for drop_index in range(drop_count):
            drop_entries.extend(
                struct.pack(
                    ">IIHii",
                    2700 + drop_index,
                    101 + drop_index,
                    1,
                    300 + drop_index,
                    -300 - drop_index,
                )
            )
        drop_payload = struct.pack(">IIH", room_id, 12345, drop_count) + bytes(drop_entries)
        for offset in range(self.room_size):
            _send_packet(self._clients[first + offset], TYPE_MONSTER_DEATH, death_payload)
            _send_packet(
                self._clients[first + offset],
                TYPE_DROP_LIST_SNAPSHOT_V2,
                drop_payload,
            )

    def _recv_rudp_click_loots_and_send_resolved(
        self,
        room_id: int,
        first: int,
        cycle_index: int,
    ) -> None:
        expected_cmd_seq = (
            self._first_ready_cmd_seq_for_cycle(cycle_index) +
            1 +
            self.expected_rudp_move_packets_per_player +
            self.expected_rudp_attack_packets_per_player
        )
        drop_count = 1 if self.single_drop_after_attack_death else max(1, self.room_size - 1)
        for drop_index in range(drop_count):
            click_loots = 0
            click_endpoints: list[tuple[str, int]] = []
            expected_drop_id = 2700 + drop_index
            expected_clicks = self.room_size if drop_index == 0 else 1
            while click_loots < expected_clicks:
                packet, endpoint = self._udp.recvfrom(2048)
                self.observed_rudp_packet_types.append(_rudp_packet_type(packet))
                client_id = _rudp_client_id(packet)
                if client_id is not None:
                    self.observed_rudp_client_ids.append(client_id)
                payload = _rudp_payload(packet)
                if _rudp_packet_type(packet) != RUDP_TYPE_INPUT_COMMAND:
                    continue
                if len(payload) != 14:
                    raise AssertionError("invalid RUDP ClickLoot payload size")
                player_id, cmd_seq, op, arg_len, drop_id = struct.unpack(
                    ">IIBBI",
                    payload,
                )
                if (
                    player_id == 0 or
                    cmd_seq != expected_cmd_seq + drop_index or
                    op != RUDP_INPUT_CLICK_LOOT or
                    arg_len != 4
                ):
                    raise AssertionError("invalid RUDP ClickLoot payload")
                if drop_id != expected_drop_id:
                    raise AssertionError("invalid RUDP ClickLoot target")
                click_endpoints.append(endpoint)
                click_loots += 1

            payload = struct.pack(
                ">HHIIQIH",
                0x0002,
                22,
                room_id,
                expected_drop_id,
                1000 + first,
                101 + drop_index,
                1,
            )
            fanout_endpoints = self._udp_endpoints_by_room_cycle[(cycle_index, first)]
            for offset, endpoint in enumerate(fanout_endpoints):
                self._udp.sendto(
                    _rudp_packet(
                        channel=RUDP_CHANNEL_EVENT,
                        packet_type=RUDP_TYPE_GAME_EVENT,
                        sequence=10000 + (drop_index * self.room_size) + offset,
                        payload=payload,
                        flags=RUDP_FLAG_RELIABLE,
                    ),
                    endpoint,
                )
        if self.send_battle_result_after_loot:
            self._send_battle_result(room_id, first)

    def _send_battle_result(self, room_id: int, first: int) -> None:
        rows = bytearray()
        for row_offset in range(self.room_size):
            session_id = 1000 + first + row_offset
            nickname = f"Player{session_id}".encode("utf-8")
            total_asset_value = 100 if row_offset == 0 else 0
            rows.extend(struct.pack(">HQB", row_offset + 1, session_id, len(nickname)))
            rows.extend(nickname)
            rows.extend(struct.pack(">q", total_asset_value))

        ranking_payload = (
            struct.pack(">IQB", room_id, 900000 + room_id, self.room_size) +
            bytes(rows)
        )
        lobby_return_payload = struct.pack(">IB", room_id, 0)
        room_list_payload = struct.pack(">H", 0)
        for offset in range(self.room_size):
            client = self._clients[first + offset]
            _send_packet(client, TYPE_BATTLE_FINAL_RANKING, ranking_payload)
            _send_packet(client, TYPE_LOBBY_RETURN_VISIBILITY, lobby_return_payload)
            _send_packet(client, TYPE_ROOM_LIST_SNAPSHOT, room_list_payload)


def _send_room_status(
    client: socket.socket,
    packet_type: int,
    room_id: int,
    player_count: int,
) -> None:
    _send_packet(client, packet_type, struct.pack(">IH", room_id, player_count))


def _send_empty_room_list_snapshots(client: socket.socket, count: int) -> None:
    for _ in range(count):
        _send_packet(client, TYPE_ROOM_LIST_SNAPSHOT, struct.pack(">H", 0))


def _send_packet(client: socket.socket, packet_type: int, payload: bytes) -> None:
    client.sendall(struct.pack(">HH", 4 + len(payload), packet_type) + payload)


def _recv_packet(client: socket.socket) -> bytes:
    header = _recv_exact(client, 4)
    size, _ = struct.unpack(">HH", header)
    return header + _recv_exact(client, size - 4)


def _recv_exact(client: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = client.recv(size - len(chunks))
        if not chunk:
            raise ConnectionError("socket closed")
        chunks.extend(chunk)
    return bytes(chunks)


def _tcp_packet_waiting(client: socket.socket) -> bool:
    previous_timeout = client.gettimeout()
    client.settimeout(0.01)
    try:
        return bool(client.recv(1, socket.MSG_PEEK))
    except (TimeoutError, socket.timeout, BlockingIOError):
        return False
    finally:
        client.settimeout(previous_timeout)


def _packet_type(packet: bytes) -> int:
    return struct.unpack(">H", packet[2:4])[0]


def _auth_token(packet: bytes) -> str:
    if _packet_type(packet) != TYPE_AUTHENTICATE_GAME_SESSION:
        raise AssertionError("expected AuthenticateGameSession packet")
    token_len = struct.unpack(">H", packet[4:6])[0]
    return packet[6:6 + token_len].decode("utf-8")


def _rudp_packet_type(packet: bytes) -> int:
    return struct.unpack(">H", packet[6:8])[0]


def _rudp_client_id(packet: bytes):
    packet_type = _rudp_packet_type(packet)
    payload = _rudp_payload(packet)
    if packet_type == RUDP_TYPE_HELLO and len(payload) >= 6:
        return struct.unpack(">HI", payload[:6])[1]
    if packet_type == RUDP_TYPE_INPUT_COMMAND and len(payload) >= 4:
        return struct.unpack(">I", payload[:4])[0]
    return None


def _rudp_payload(packet: bytes) -> bytes:
    payload_len = struct.unpack(">H", packet[20:22])[0]
    return packet[28:28 + payload_len]


def _rudp_packet(
    channel: int,
    packet_type: int,
    sequence: int,
    payload: bytes,
    flags: int = 0,
    ack: int = 0,
) -> bytes:
    packet = bytearray(28 + len(payload))
    packet[0:2] = b"LO"
    packet[2] = 1
    packet[3] = flags
    packet[4] = 28
    packet[5] = channel
    struct.pack_into(">H", packet, 6, packet_type)
    struct.pack_into(">I", packet, 8, sequence)
    struct.pack_into(">I", packet, 12, ack)
    struct.pack_into(">I", packet, 16, 0)
    struct.pack_into(">H", packet, 20, len(payload))
    struct.pack_into(">I", packet, 22, 0)
    struct.pack_into(">H", packet, 26, 0)
    packet[28:] = payload
    checksum = zlib.crc32(packet) & 0xFFFFFFFF
    struct.pack_into(">I", packet, 22, checksum)
    return bytes(packet)


if __name__ == "__main__":
    unittest.main()
