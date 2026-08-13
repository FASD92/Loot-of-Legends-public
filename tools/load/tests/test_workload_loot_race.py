from __future__ import annotations

import unittest


try:
    from tools.load.loot_load.protocol.rudp import RudpMessage
    from tools.load.loot_load.protocol.tcp import TcpMessage
    from tools.load.loot_load.workload.loot_race import (
        LootRaceCoordinator,
        PlayerPhase,
        WorkloadViolation,
    )
except ModuleNotFoundError:
    RudpMessage = None
    TcpMessage = None
    LootRaceCoordinator = None
    PlayerPhase = None
    WorkloadViolation = None


def tcp(name, **fields):
    return TcpMessage(name, fields)


def rudp(player_index, name, **fields):
    return RudpMessage(
        name,
        {
            "flag": 1,
            "sessionId": player_index + 1,
            "sessionGeneration": 1,
            "transportEpoch": 3,
            "sequence": 1,
            "ack": 0,
            "ackBits": 0,
            "messageId": 0,
        },
        fields,
    )


def members(*, ready):
    return [
        {
            "sessionId": index + 1,
            "sessionGeneration": 1,
            "nickname": f"p{index:02d}",
            "ready": ready,
        }
        for index in range(10)
    ]


def room_detail(*, ready, room_id=7):
    return tcp(
        "RoomDetailProjection",
        roomId=room_id,
        title="load-room",
        capacity=10,
        hostSessionId=1,
        hostSessionGeneration=1,
        members=members(ready=ready),
    )


def gameplay_start(battle_id):
    return tcp(
        "ArenaGameplayStart",
        roomId=7,
        battleInstanceId=battle_id,
        participants=[
            {
                "sessionId": index + 1,
                "sessionGeneration": 1,
                "nickname": f"p{index:02d}",
            }
            for index in range(10)
        ],
    )


class LootRaceWorkloadTests(unittest.TestCase):
    def setUp(self) -> None:
        if LootRaceCoordinator is None:
            self.fail("Loot Race workload module is absent")
        self.coordinator = LootRaceCoordinator(seed=20260811)

    def authenticate_and_form_room(self):
        for index in range(10):
            self.coordinator.on_tcp(
                index,
                tcp(
                    "Welcome",
                    requestId=index + 1,
                    sessionId=index + 1,
                    sessionGeneration=1,
                    serverTimeUnixMillis=1,
                    nickname=f"p{index:02d}",
                ),
                at_ms=0,
            )
        commands = self.coordinator.take_commands()
        self.assertEqual(["CreateRoom"], [command.name for command in commands])

        host_only = tcp(
            "RoomDetailProjection",
            roomId=7,
            title="load-room",
            capacity=10,
            hostSessionId=1,
            hostSessionGeneration=1,
            members=members(ready=False)[:1],
        )
        self.coordinator.on_tcp(0, host_only, at_ms=10)
        commands = self.coordinator.take_commands()
        self.assertEqual(9, len(commands))
        self.assertTrue(all(command.name == "JoinRoom" for command in commands))

        for index in range(10):
            self.coordinator.on_tcp(index, room_detail(ready=False), at_ms=20)
        commands = self.coordinator.take_commands()
        self.assertEqual(10, len(commands))
        self.assertTrue(all(command.name == "SetReady" for command in commands))

        for index in range(10):
            self.coordinator.on_tcp(index, room_detail(ready=True), at_ms=30)
        commands = self.coordinator.take_commands()
        self.assertEqual(["HostStartRequest"], [command.name for command in commands])

    def enter_gameplay(self, battle_id, at_ms):
        for index in range(10):
            self.coordinator.on_tcp(
                index,
                tcp("ArenaLoadEntry", roomId=7, battleInstanceId=battle_id),
                at_ms=at_ms,
            )
            self.coordinator.on_rudp(
                index, rudp(index, "RudpBindAccepted"), at_ms=at_ms
            )
        commands = self.coordinator.take_commands()
        self.assertEqual(10, sum(command.name == "ArenaLoadComplete" for command in commands))
        for index in range(10):
            self.coordinator.on_tcp(index, gameplay_start(battle_id), at_ms=at_ms + 10)

    def test_exact_room_load_bind_and_non_oracle_gameplay_schedule(self):
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)
        self.coordinator.tick(110)
        self.coordinator.tick(210)
        self.coordinator.tick(310)
        moves = [
            command
            for command in self.coordinator.take_commands()
            if command.name == "MoveIntent"
        ]
        self.assertEqual(30, len(moves))
        self.assertEqual(
            (moves[0].fields["desiredX"], moves[0].fields["desiredY"]),
            (moves[10].fields["desiredX"], moves[10].fields["desiredY"]),
        )
        self.assertNotEqual(
            (moves[10].fields["desiredX"], moves[10].fields["desiredY"]),
            (moves[20].fields["desiredX"], moves[20].fields["desiredY"]),
        )

        for index in range(10):
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "MonsterSpawned",
                    eventId={"high": 1, "low": index + 1},
                    battleInstanceId=1,
                    eventStreamKind="COMBAT_LIFECYCLE",
                    eventSequence=1,
                    monsterId=1,
                    posXMillimeter=0,
                    posYMillimeter=0,
                    maximumHitPoints=1600,
                    rulesetVersion=1,
                ),
                at_ms=320,
            )
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "StateSnapshot",
                    battleInstanceId=1,
                    snapshotSequence=1,
                    serverTick=1,
                    players=[
                        {
                            "sessionId": index + 1,
                            "posXMillimeter": 1000,
                            "posYMillimeter": 1000,
                        }
                    ],
                ),
                at_ms=320,
            )
        self.coordinator.tick(860)
        attacks = [
            command
            for command in self.coordinator.take_commands()
            if command.name == "AttackIntent"
        ]
        self.assertEqual(10, len(attacks))
        self.assertTrue(all(command.fields["targetHint"] == 1 for command in attacks))
        self.assertTrue(
            all(
                set(command.fields) == {"commandId", "battleInstanceId", "targetHint"}
                for command in attacks
            )
        )

        self.coordinator.tick(1060)
        retries = [
            command
            for command in self.coordinator.take_commands()
            if command.name == "AttackIntent"
        ]
        self.assertEqual(
            [attack.fields["commandId"] for attack in attacks],
            [retry.fields["commandId"] for retry in retries],
        )
        self.assertTrue(all(retry.application_attempt == 2 for retry in retries))

    def test_monster_spawn_before_tcp_gameplay_start_still_schedules_attack(self):
        self.authenticate_and_form_room()
        for index in range(10):
            self.coordinator.on_tcp(
                index,
                tcp("ArenaLoadEntry", roomId=7, battleInstanceId=1),
                at_ms=100,
            )
            self.coordinator.on_rudp(
                index, rudp(index, "RudpBindAccepted"), at_ms=100
            )
        self.coordinator.take_commands()

        for index in range(10):
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "MonsterSpawned",
                    eventId={"high": 1, "low": index + 1},
                    battleInstanceId=1,
                    eventStreamKind="COMBAT_LIFECYCLE",
                    eventSequence=1,
                    monsterId=1,
                    posXMillimeter=0,
                    posYMillimeter=0,
                    maximumHitPoints=1600,
                    rulesetVersion=1,
                ),
                at_ms=109,
            )
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "StateSnapshot",
                    battleInstanceId=1,
                    snapshotSequence=1,
                    serverTick=1,
                    players=[
                        {
                            "sessionId": index + 1,
                            "posXMillimeter": 0,
                            "posYMillimeter": 0,
                        }
                    ],
                ),
                at_ms=109,
            )
            self.coordinator.on_tcp(index, gameplay_start(1), at_ms=110)

        self.coordinator.tick(860)
        attacks = [
            command
            for command in self.coordinator.take_commands()
            if command.name == "AttackIntent"
        ]
        self.assertEqual(10, len(attacks))
        self.assertTrue(all(command.fields["targetHint"] == 1 for command in attacks))

    def test_reopen_does_not_count_cycle_without_attack_and_loot(self):
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)

        self.finish_cycle(1, at_ms=1000)

        self.assertFalse(self.coordinator.complete)
        self.assertEqual((), self.coordinator.completed_battle_ids)
        self.assertEqual([], self.coordinator.take_commands())

    def test_combat_timeout_does_not_count_as_canonical_loot_race(self):
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)
        for index in range(10):
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "CombatTerminalEvent",
                    eventId={"high": 2, "low": index + 1},
                    battleInstanceId=1,
                    eventStreamKind="COMBAT_LIFECYCLE",
                    eventSequence=2,
                    combatOutcome="COMBAT_TIMEOUT",
                    monsterId=1,
                    serverTick=10,
                    rulesetVersion=1,
                ),
                at_ms=500,
            )
            self.coordinator.on_tcp(
                index,
                tcp(
                    "FinalResult",
                    roomId=7,
                    battleInstanceId=1,
                    outcome="COMBAT_TIMEOUT",
                    entries=[],
                ),
                at_ms=600,
            )
        for index in range(10):
            self.coordinator.on_tcp(index, room_detail(ready=False), at_ms=601)

        self.assertFalse(self.coordinator.complete)
        self.assertEqual((), self.coordinator.completed_battle_ids)
        self.assertEqual([], self.coordinator.take_commands())

    def test_room_reopen_before_other_connections_final_result_still_completes(self):
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)
        self.exercise_combat(1, at_ms=900)
        self.exercise_loot(1, at_ms=1000)
        for index in range(10):
            self.coordinator.on_tcp(index, room_detail(ready=False), at_ms=2100)
        for index in range(10):
            self.coordinator.on_tcp(
                index,
                tcp(
                    "FinalResult",
                    roomId=7,
                    battleInstanceId=1,
                    outcome="MONSTER_DEFEATED",
                    entries=[],
                ),
                at_ms=2101,
            )

        self.assertEqual((1,), self.coordinator.completed_battle_ids)
        self.assertEqual(10, len(self.coordinator.take_commands()))

    def test_loot_moves_to_server_projection_and_retries_with_new_command(self):
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)
        for index in range(10):
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "StateSnapshot",
                    battleInstanceId=1,
                    snapshotSequence=1,
                    serverTick=1,
                    players=[
                        {
                            "sessionId": index + 1,
                            "posXMillimeter": 0,
                            "posYMillimeter": 0,
                        }
                    ],
                ),
                at_ms=490,
            )
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "CombatTerminalEvent",
                    eventId={"high": 2, "low": index + 1},
                    battleInstanceId=1,
                    eventStreamKind="COMBAT_LIFECYCLE",
                    eventSequence=2,
                    combatOutcome="MONSTER_DEFEATED",
                    monsterId=1,
                    serverTick=10,
                    rulesetVersion=1,
                ),
                at_ms=500,
            )
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "DropStateSnapshot",
                    battleInstanceId=1,
                    snapshotSequence=1,
                    resolutionState="OPEN",
                    drops=[
                        {
                            "dropId": 2,
                            "itemId": 1,
                            "quantity": 1,
                            "posXMillimeter": 8000,
                            "posYMillimeter": 8000,
                            "state": "AVAILABLE",
                            "ownerSessionId": 0,
                        }
                    ],
                ),
                at_ms=500,
            )
            self.coordinator.on_tcp(index, room_detail(ready=True), at_ms=501)

        self.coordinator.tick(500)
        commands = self.coordinator.take_commands()
        self.assertFalse(any(command.name == "ClaimLootIntent" for command in commands))
        moves = [
            command
            for command in commands
            if command.name == "MoveIntent" and command.participant_index == 0
        ]
        self.assertTrue(moves)
        self.assertEqual(
            (23170, 23170),
            (moves[-1].fields["desiredX"], moves[-1].fields["desiredY"]),
        )

        self.coordinator.on_rudp(
            0,
            rudp(
                0,
                "StateSnapshot",
                battleInstanceId=1,
                snapshotSequence=2,
                serverTick=2,
                players=[
                    {
                        "sessionId": 1,
                        "posXMillimeter": 7000,
                        "posYMillimeter": 7000,
                    }
                ],
            ),
            at_ms=600,
        )
        self.coordinator.tick(600)
        first = next(
            command
            for command in self.coordinator.take_commands()
            if command.name == "ClaimLootIntent" and command.participant_index == 0
        )
        self.assertEqual(600, first.at_ms)

        self.coordinator.on_rudp(
            0,
            rudp(
                0,
                "ClaimLootTerminalResult",
                commandId=first.fields["commandId"],
                battleInstanceId=1,
                dropId=2,
                resultCode="OUT_OF_RANGE",
            ),
            at_ms=610,
        )
        self.coordinator.tick(810)
        second = next(
            command
            for command in self.coordinator.take_commands()
            if command.name == "ClaimLootIntent" and command.participant_index == 0
        )
        self.assertNotEqual(first.fields["commandId"], second.fields["commandId"])

    def test_successful_claim_does_not_compete_for_second_drop(self):
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)
        participant = 8
        self.coordinator.on_rudp(
            participant,
            rudp(
                participant,
                "StateSnapshot",
                battleInstanceId=1,
                snapshotSequence=1,
                serverTick=1,
                players=[
                    {
                        "sessionId": participant + 1,
                        "posXMillimeter": 0,
                        "posYMillimeter": 0,
                    }
                ],
            ),
            at_ms=500,
        )
        self.coordinator.on_rudp(
            participant,
            rudp(
                participant,
                "CombatTerminalEvent",
                eventId={"high": 2, "low": participant + 1},
                battleInstanceId=1,
                eventStreamKind="COMBAT_LIFECYCLE",
                eventSequence=2,
                combatOutcome="MONSTER_DEFEATED",
                monsterId=1,
                serverTick=10,
                rulesetVersion=1,
            ),
            at_ms=500,
        )
        self.coordinator.on_rudp(
            participant,
            rudp(
                participant,
                "DropStateSnapshot",
                battleInstanceId=1,
                snapshotSequence=1,
                resolutionState="OPEN",
                drops=[
                    {
                        "dropId": drop_id,
                        "itemId": 1,
                        "quantity": 1,
                        "posXMillimeter": 0,
                        "posYMillimeter": 0,
                        "state": "AVAILABLE",
                        "ownerSessionId": 0,
                    }
                    for drop_id in range(1, 11)
                ],
            ),
            at_ms=500,
        )

        self.coordinator.tick(1300)
        first = next(
            command
            for command in self.coordinator.take_commands()
            if command.name == "ClaimLootIntent"
            and command.participant_index == participant
        )
        self.assertEqual(4, first.fields["dropId"])
        self.coordinator.on_rudp(
            participant,
            rudp(
                participant,
                "ClaimLootTerminalResult",
                commandId=first.fields["commandId"],
                battleInstanceId=1,
                dropId=4,
                resultCode="OK",
            ),
            at_ms=1301,
        )
        self.coordinator.on_rudp(
            participant,
            rudp(
                participant,
                "DropStateSnapshot",
                battleInstanceId=1,
                snapshotSequence=2,
                resolutionState="OPEN",
                drops=[
                    {
                        "dropId": 4,
                        "itemId": 1,
                        "quantity": 1,
                        "posXMillimeter": 0,
                        "posYMillimeter": 0,
                        "state": "CLAIMED",
                        "ownerSessionId": participant + 1,
                    },
                    {
                        "dropId": 6,
                        "itemId": 1,
                        "quantity": 1,
                        "posXMillimeter": 0,
                        "posYMillimeter": 0,
                        "state": "AVAILABLE",
                        "ownerSessionId": 0,
                    },
                ],
            ),
            at_ms=1302,
        )

        self.coordinator.tick(1501)
        claims = [
            command
            for command in self.coordinator.take_commands()
            if command.name == "ClaimLootIntent"
            and command.participant_index == participant
        ]
        self.assertEqual([], claims)

    def test_server_owned_loot_stagger_retry_and_same_room_second_cycle(self):
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)
        self.exercise_combat(1, at_ms=900)
        for index in range(10):
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "DropStateSnapshot",
                    battleInstanceId=1,
                    snapshotSequence=1,
                    resolutionState="OPEN",
                    drops=[
                        {
                            "dropId": drop_id,
                            "itemId": 1,
                            "quantity": 1,
                            "posXMillimeter": 0,
                            "posYMillimeter": 0,
                            "state": "AVAILABLE",
                            "ownerSessionId": 0,
                        }
                        for drop_id in (2, 5)
                    ],
                ),
                at_ms=1000,
            )
        commands = []
        for at_ms in range(1000, 2000, 100):
            self.coordinator.tick(at_ms)
            commands.extend(self.coordinator.take_commands())
        claims = [command for command in commands if command.name == "ClaimLootIntent"]
        first = [command for command in claims if command.application_attempt == 1]
        self.assertEqual(10, len(first))
        self.assertEqual(list(range(1000, 2000, 100)), [claim.at_ms for claim in first])
        self.assertTrue(all(claim.fields["dropId"] in {2, 5} for claim in first))
        first_by_participant = {
            claim.participant_index: claim.fields["commandId"] for claim in first
        }
        retries = [command for command in claims if command.application_attempt > 1]
        self.assertTrue(retries)
        self.assertTrue(
            all(
                retry.fields["commandId"]
                == first_by_participant[retry.participant_index]
                for retry in retries
            )
        )
        for index, player in enumerate(self.coordinator.players):
            self.assertIsNotNone(player.pending)
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "ClaimLootTerminalResult",
                    commandId=player.pending.fields["commandId"],
                    battleInstanceId=1,
                    dropId=player.pending.fields["dropId"],
                    resultCode="ALREADY_CLAIMED",
                ),
                at_ms=2000,
            )

        self.finish_cycle(1, at_ms=2100)
        ready = self.coordinator.take_commands()
        self.assertEqual(10, sum(command.name == "SetReady" for command in ready))
        self.enter_gameplay(2, 2200)
        self.complete_cycle(2, combat_at_ms=3000)
        self.assertTrue(self.coordinator.complete)
        self.assertEqual((1, 2), self.coordinator.completed_battle_ids)
        self.assertTrue(all(player.phase == PlayerPhase.REOPENED for player in self.coordinator.players))

    def test_second_cycle_rejects_reused_battle_instance(self):
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)
        self.complete_cycle(1, combat_at_ms=900)
        self.coordinator.take_commands()
        with self.assertRaises(WorkloadViolation):
            self.coordinator.on_tcp(
                0,
                tcp("ArenaLoadEntry", roomId=7, battleInstanceId=1),
                at_ms=1100,
            )

    def test_profile_required_cycle_count_is_not_hard_coded_to_two(self):
        self.coordinator = LootRaceCoordinator(seed=20260811, required_cycles=4)
        self.authenticate_and_form_room()
        for battle_id in range(1, 5):
            started_at = battle_id * 3000
            self.enter_gameplay(battle_id, started_at)
            self.complete_cycle(battle_id, combat_at_ms=started_at + 800)
            self.coordinator.take_commands()
            self.assertEqual(battle_id == 4, self.coordinator.complete)
        self.assertEqual((1, 2, 3, 4), self.coordinator.completed_battle_ids)

    def test_drain_finishes_current_cycle_without_starting_another(self):
        self.coordinator = LootRaceCoordinator(seed=20260811, required_cycles=30)
        self.authenticate_and_form_room()
        self.enter_gameplay(1, 100)
        self.coordinator.request_drain()
        self.complete_cycle(1, combat_at_ms=900)
        self.assertTrue(self.coordinator.drained)
        self.assertEqual([], self.coordinator.take_commands())

    def exercise_combat(self, battle_id, at_ms):
        for index in range(10):
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "MonsterSpawned",
                    eventId={"high": battle_id, "low": index + 1},
                    battleInstanceId=battle_id,
                    eventStreamKind="COMBAT_LIFECYCLE",
                    eventSequence=1,
                    monsterId=1,
                    posXMillimeter=0,
                    posYMillimeter=0,
                    maximumHitPoints=1600,
                    rulesetVersion=1,
                ),
                at_ms=at_ms - 1,
            )
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "StateSnapshot",
                    battleInstanceId=battle_id,
                    snapshotSequence=1,
                    serverTick=1,
                    players=[
                        {
                            "sessionId": index + 1,
                            "posXMillimeter": 0,
                            "posYMillimeter": 0,
                        }
                    ],
                ),
                at_ms=at_ms - 1,
            )
        self.coordinator.tick(at_ms)
        attacks = [
            command
            for command in self.coordinator.take_commands()
            if command.name == "AttackIntent"
        ]
        self.assertEqual(10, len(attacks))
        for attack in attacks:
            index = attack.participant_index
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "AttackTerminalResult",
                    commandId=attack.fields["commandId"],
                    battleInstanceId=battle_id,
                    resultCode="OK",
                    monsterId=1,
                    remainingHitPoints=100,
                    rulesetVersion=1,
                    combatOutcome="NONE",
                ),
                at_ms=at_ms + 1,
            )
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "CombatTerminalEvent",
                    eventId={"high": battle_id, "low": index + 1},
                    battleInstanceId=battle_id,
                    eventStreamKind="COMBAT_LIFECYCLE",
                    eventSequence=2,
                    combatOutcome="MONSTER_DEFEATED",
                    monsterId=1,
                    serverTick=10,
                    rulesetVersion=1,
                ),
                at_ms=at_ms + 2,
            )

    def exercise_loot(self, battle_id, at_ms):
        for index in range(10):
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "DropStateSnapshot",
                    battleInstanceId=battle_id,
                    snapshotSequence=1,
                    resolutionState="OPEN",
                    drops=[
                        {
                            "dropId": drop_id,
                            "itemId": 1,
                            "quantity": 1,
                            "posXMillimeter": 0,
                            "posYMillimeter": 0,
                            "state": "AVAILABLE",
                            "ownerSessionId": 0,
                        }
                        for drop_id in range(1, 11)
                    ],
                ),
                at_ms=at_ms,
            )
        claims = []
        for tick_at in range(at_ms, at_ms + 1000, 100):
            self.coordinator.tick(tick_at)
            claims.extend(
                command
                for command in self.coordinator.take_commands()
                if command.name == "ClaimLootIntent"
                and command.application_attempt == 1
            )
        self.assertEqual(10, len(claims))
        self.assertEqual(10, len({claim.fields["dropId"] for claim in claims}))
        for claim in claims:
            index = claim.participant_index
            self.coordinator.on_rudp(
                index,
                rudp(
                    index,
                    "ClaimLootTerminalResult",
                    commandId=claim.fields["commandId"],
                    battleInstanceId=battle_id,
                    dropId=claim.fields["dropId"],
                    resultCode="ALREADY_CLAIMED",
                ),
                at_ms=at_ms + 1000,
            )

    def complete_cycle(self, battle_id, combat_at_ms):
        self.exercise_combat(battle_id, combat_at_ms)
        self.exercise_loot(battle_id, combat_at_ms + 100)
        self.finish_cycle(battle_id, at_ms=combat_at_ms + 1200)

    def finish_cycle(self, battle_id, at_ms):
        for index in range(10):
            self.coordinator.on_tcp(
                index,
                tcp(
                    "FinalResult",
                    roomId=7,
                    battleInstanceId=battle_id,
                    outcome="MONSTER_DEFEATED",
                    entries=[
                        {
                            "sessionId": participant + 1,
                            "nickname": f"p{participant:02d}",
                            "exitStatus": "TERMINAL_PRESENT",
                            "finalAssetValue": 10 - participant,
                            "rank": participant + 1,
                            "isTop": participant == 0,
                        }
                        for participant in range(10)
                    ],
                ),
                at_ms=at_ms,
            )
        for index in range(10):
            self.coordinator.on_tcp(index, room_detail(ready=False), at_ms=at_ms + 1)


if __name__ == "__main__":
    unittest.main()
