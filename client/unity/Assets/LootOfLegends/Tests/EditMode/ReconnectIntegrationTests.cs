using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Bootstrap;
using LootOfLegends.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation;
using LootOfLegends.Presentation.Room;
using LootOfLegends.Protocol;
using LootOfLegends.Session;
using LootOfLegends.Transport;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class ReconnectIntegrationTests
    {
        [Test]
        public void CrashEvidenceDriverAcceptsModeAndCurrentGenerationCallback()
        {
            var load = ActiveLoad();
            ConstructorInfo constructor = typeof(DevelopmentPlayerFlowDriver)
                .GetConstructor(
                    BindingFlags.Instance | BindingFlags.Public,
                    null,
                    new[]
                    {
                        typeof(string),
                        typeof(LobbyRoomReadModel),
                        typeof(ILobbyRoomCommands),
                        typeof(IRoomHostStartAction),
                        typeof(BattleLoadReadModel),
                        typeof(BattleResultReadModel),
                        typeof(Func<ArenaInputBinding>),
                        typeof(ICollectionApi),
                        typeof(CollectionReadModel),
                        typeof(CancellationToken),
                        typeof(bool),
                        typeof(Func<ulong>)
                    },
                    null);

            Assert.That(constructor, Is.Not.Null);
            var driver = (DevelopmentPlayerFlowDriver)constructor.Invoke(
                new object[]
                {
                    "host",
                    new LobbyRoomReadModel(),
                    new StubRoomCommands(),
                    new StubHostStart(),
                    load,
                    new BattleResultReadModel(),
                    new Func<ArenaInputBinding>(() => null),
                    new StubCollectionApi(),
                    new CollectionReadModel(),
                    CancellationToken.None,
                    true,
                    new Func<ulong>(() => 13)
                });

            Assert.That(GetField(driver, "crashContinuityEvidence"), Is.EqualTo(true));
            var generation = (Func<ulong>)GetField(driver, "sessionGeneration");
            Assert.That(generation(), Is.EqualTo(13UL));
        }

        [Test]
        public void CrashEvidenceKeepsBoundedLootFailureStage()
        {
            MethodInfo method = typeof(DevelopmentPlayerFlowDriver).GetMethod(
                "SafeFailureStage",
                BindingFlags.Static | BindingFlags.NonPublic);

            Assert.That(method, Is.Not.Null);
            Assert.That(
                method.Invoke(null, new object[] { "loot_claim_not_confirmed" }),
                Is.EqualTo("loot_claim_not_confirmed"));
            Assert.That(
                method.Invoke(null, new object[] { "unexpected_detail" }),
                Is.EqualTo("runtime"));
        }

        [Test]
        public void CrashEvidenceSnapshotAndAckBoundariesAreOrderedAndExactlyOnce()
        {
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var load = ActiveLoad();
                var lobby = new LobbyRoomReadModel();
                lobby.Apply(new LobbyEntrySnapshot(
                    1,
                    2,
                    "neo",
                    Array.Empty<RoomSummary>()));
                ArenaClientRuntime runtime = CreateArenaRuntime(load, socket);
                var outbound = (RudpReliableOutbound)GetField(
                    runtime.Movement,
                    "reliableOutbound");
                SetField(outbound, "transportEpoch", (uint)1);
                runtime.Movement.ReadModel.Apply(new RudpStateSnapshot(
                    9,
                    1,
                    20,
                    new[] { new RudpSnapshotPlayer(1, 10, 20) }));
                var binding = new ArenaInputBinding(
                    runtime.Movement,
                    runtime.Movement.ReadModel,
                    runtime.Combat,
                    runtime.Loot,
                    runtime.Presentation,
                    runtime.Input,
                    1);
                ulong generation = 2;
                var driver = new DevelopmentPlayerFlowDriver(
                    "host",
                    lobby,
                    new StubRoomCommands(),
                    new StubHostStart(),
                    load,
                    new BattleResultReadModel(),
                    () => binding,
                    new StubCollectionApi(),
                    new CollectionReadModel(),
                    CancellationToken.None,
                    true,
                    () => generation);
                SetField(driver, "preCrashObserved", true);
                load.SetReconnectLocked(true);

                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=reconnect_started " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(driver, "OnReconnectStarted");
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=snapshot_applied " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(driver, "OnSnapshotApplied");
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=snapshot_acknowledged " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(driver, "OnSnapshotAcknowledged");

                Assert.That(GetField(driver, "snapshotAppliedObserved"), Is.EqualTo(true));
                Assert.That(
                    GetField(driver, "snapshotAcknowledgedObserved"),
                    Is.EqualTo(true));
                generation = 3;
                load.SetReconnectLocked(false);
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=post_reconnect_state " +
                    "role=host session=1 room=7 battle=9 generation=3 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Assert.That(
                    Invoke(driver, "TryObservePostReconnect", binding),
                    Is.EqualTo(true));
            }
        }

        [Test]
        public void CrashEvidenceSnapshotAndAckCallbacksRejectInvalidOrderAndDuplicates()
        {
            var outOfOrder = CreateCrashDriver(ActiveLoad(), () => 2);
            SetField(outOfOrder, "reconnectStarted", true);
            LogAssert.ignoreFailingMessages = true;
            try
            {
                Invoke(outOfOrder, "OnSnapshotAcknowledged");
            }
            finally
            {
                LogAssert.ignoreFailingMessages = false;
            }
            Assert.That(GetField(outOfOrder, "finished"), Is.EqualTo(true));

            var duplicateSnapshot = CreateCrashDriver(ActiveLoad(), () => 2);
            SetField(duplicateSnapshot, "reconnectStarted", true);
            LogAssert.ignoreFailingMessages = true;
            try
            {
                Invoke(duplicateSnapshot, "OnSnapshotApplied");
                Invoke(duplicateSnapshot, "OnSnapshotApplied");
            }
            finally
            {
                LogAssert.ignoreFailingMessages = false;
            }
            Assert.That(GetField(duplicateSnapshot, "finished"), Is.EqualTo(true));

            var duplicateAck = CreateCrashDriver(ActiveLoad(), () => 2);
            SetField(duplicateAck, "reconnectStarted", true);
            SetField(duplicateAck, "snapshotAppliedObserved", true);
            LogAssert.ignoreFailingMessages = true;
            try
            {
                Invoke(duplicateAck, "OnSnapshotAcknowledged");
                Invoke(duplicateAck, "OnSnapshotAcknowledged");
            }
            finally
            {
                LogAssert.ignoreFailingMessages = false;
            }
            Assert.That(GetField(duplicateAck, "finished"), Is.EqualTo(true));
        }

        [Test]
        public void CrashEvidencePreCrashMarkerRequiresChangedAuthoritativePosition()
        {
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var load = ActiveLoad();
                var lobby = new LobbyRoomReadModel();
                lobby.Apply(new LobbyEntrySnapshot(
                    1,
                    2,
                    "neo",
                    Array.Empty<RoomSummary>()));
                ArenaClientRuntime runtime = CreateArenaRuntime(load, socket);
                var binding = new ArenaInputBinding(
                    runtime.Movement,
                    runtime.Movement.ReadModel,
                    runtime.Combat,
                    runtime.Loot,
                    runtime.Presentation,
                    runtime.Input,
                    1);
                runtime.Movement.ReadModel.Apply(new RudpStateSnapshot(
                    9,
                    1,
                    20,
                    new[] { new RudpSnapshotPlayer(1, 10, 20) }));
                runtime.Combat.Apply(new RudpMonsterSpawned(
                    new RudpEventId(1, 1),
                    9,
                    RudpEventStreamKind.CombatLifecycle,
                    1,
                    1,
                    0,
                    0,
                    1600,
                    4));
                runtime.Combat.Apply(new RudpMonsterStateSnapshot(
                    9,
                    1,
                    20,
                    1,
                    1500,
                    RudpMonsterState.Alive));
                var driver = new DevelopmentPlayerFlowDriver(
                    "host",
                    lobby,
                    new StubRoomCommands(),
                    new StubHostStart(),
                    load,
                    new BattleResultReadModel(),
                    () => binding,
                    new StubCollectionApi(),
                    new CollectionReadModel(),
                    CancellationToken.None,
                    true,
                    () => 2);
                SetField(driver, "activeBattleId", 9UL);
                SetField(driver, "movementStage", 2);
                SetField(driver, "initialX", 10);
                SetField(driver, "initialY", 20);

                Invoke(driver, "TryEmitPreCrashState", binding);

                Assert.That(GetField(driver, "preCrashObserved"), Is.EqualTo(false));

                runtime.Movement.ReadModel.Apply(new RudpStateSnapshot(
                    9,
                    2,
                    21,
                    new[] { new RudpSnapshotPlayer(1, 11, 20) }));
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=pre_crash_state " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=21 " +
                    "x=11 y=20 monster_hp=1500 monster_max=1600 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(driver, "TryEmitPreCrashState", binding);

                Assert.That(GetField(driver, "preCrashObserved"), Is.EqualTo(true));
            }
        }

        [Test]
        public void CrashEvidencePostReconnectMarkerRequiresIdentityGenerationUnlockAndTransport()
        {
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var load = ActiveLoad();
                var lobby = new LobbyRoomReadModel();
                lobby.Apply(new LobbyEntrySnapshot(
                    1,
                    2,
                    "neo",
                    Array.Empty<RoomSummary>()));
                ArenaClientRuntime runtime = CreateArenaRuntime(load, socket);
                var outbound = (RudpReliableOutbound)GetField(
                    runtime.Movement,
                    "reliableOutbound");
                SetField(outbound, "transportEpoch", (uint)1);
                var binding = new ArenaInputBinding(
                    runtime.Movement,
                    runtime.Movement.ReadModel,
                    runtime.Combat,
                    runtime.Loot,
                    runtime.Presentation,
                    runtime.Input,
                    1);
                runtime.Movement.ReadModel.Apply(new RudpStateSnapshot(
                    9,
                    1,
                    20,
                    new[] { new RudpSnapshotPlayer(1, 10, 20) }));
                ulong generation = 2;
                var driver = new DevelopmentPlayerFlowDriver(
                    "host",
                    lobby,
                    new StubRoomCommands(),
                    new StubHostStart(),
                    load,
                    new BattleResultReadModel(),
                    () => binding,
                    new StubCollectionApi(),
                    new CollectionReadModel(),
                    CancellationToken.None,
                    true,
                    () => generation);
                SetField(driver, "preCrashObserved", true);
                load.SetReconnectLocked(true);
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=reconnect_started " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                driver.OnReconnectStarted();
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=snapshot_applied " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(driver, "OnSnapshotApplied");
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=snapshot_acknowledged " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(driver, "OnSnapshotAcknowledged");

                Assert.That(
                    Invoke(driver, "TryObservePostReconnect", binding),
                    Is.EqualTo(false));
                generation = 3;
                load.SetReconnectLocked(false);
                load.ApplyFinalResult(7, 9);
                Assert.That(
                    Invoke(driver, "TryObservePostReconnect", binding),
                    Is.EqualTo(false));
                load.Apply(new ArenaLoadEntry(7, 9));
                load.Apply(new ArenaGameplayStart(
                    7,
                    9,
                    new[] { new BattleParticipant(1, 2, "neo") }));
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=post_reconnect_state " +
                    "role=host session=1 room=7 battle=9 generation=3 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Assert.That(
                    Invoke(driver, "TryObservePostReconnect", binding),
                    Is.EqualTo(true));
                Assert.That(GetField(driver, "postReconnectObserved"), Is.EqualTo(true));
            }
        }

        [Test]
        public void CrashEvidencePostRecoveryMutationUsesRealMovementCommand()
        {
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var load = ActiveLoad();
                var lobby = new LobbyRoomReadModel();
                lobby.Apply(new LobbyEntrySnapshot(
                    1,
                    2,
                    "neo",
                    Array.Empty<RoomSummary>()));
                ArenaClientRuntime runtime = CreateArenaRuntime(load, socket);
                var outbound = (RudpReliableOutbound)GetField(
                    runtime.Movement,
                    "reliableOutbound");
                SetField(outbound, "transportEpoch", (uint)1);
                var binding = new ArenaInputBinding(
                    runtime.Movement,
                    runtime.Movement.ReadModel,
                    runtime.Combat,
                    runtime.Loot,
                    runtime.Presentation,
                    runtime.Input,
                    1);
                runtime.Movement.ReadModel.Apply(new RudpStateSnapshot(
                    9,
                    1,
                    20,
                    new[] { new RudpSnapshotPlayer(1, 10, 20) }));
                ulong generation = 2;
                var driver = new DevelopmentPlayerFlowDriver(
                    "host",
                    lobby,
                    new StubRoomCommands(),
                    new StubHostStart(),
                    load,
                    new BattleResultReadModel(),
                    () => binding,
                    new StubCollectionApi(),
                    new CollectionReadModel(),
                    CancellationToken.None,
                    true,
                    () => generation);
                SetField(driver, "preCrashObserved", true);
                load.SetReconnectLocked(true);
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=reconnect_started " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                driver.OnReconnectStarted();
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=snapshot_applied " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(driver, "OnSnapshotApplied");
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=snapshot_acknowledged " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(driver, "OnSnapshotAcknowledged");
                generation = 3;
                load.SetReconnectLocked(false);
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=post_reconnect_state " +
                    "role=host session=1 room=7 battle=9 generation=3 tick=20 " +
                    "x=10 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Assert.That(
                    Invoke(driver, "TryObservePostReconnect", binding),
                    Is.EqualTo(true));

                Invoke(
                    driver,
                    "TickPostRecoveryMutation",
                    binding,
                    Time.realtimeSinceStartupAsDouble);
                var movementOperation = (Task)GetField(driver, "operation");
                Assert.That(movementOperation.Wait(1000), Is.True);
                Assert.That(
                    GetField(driver, "operationStage"),
                    Is.EqualTo("post_recovery_movement"));
                Assert.That(Invoke(driver, "ObserveOperation"), Is.EqualTo(true));

                runtime.Movement.ReadModel.Apply(new RudpStateSnapshot(
                    9,
                    2,
                    21,
                    new[] { new RudpSnapshotPlayer(1, 11, 20) }));
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=post_recovery_mutation " +
                    "role=host session=1 room=7 battle=9 generation=3 tick=21 " +
                    "x=11 y=20 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=1 remaining_s=30");
                Invoke(
                    driver,
                    "TickPostRecoveryMutation",
                    binding,
                    Time.realtimeSinceStartupAsDouble);

                Assert.That(
                    GetField(driver, "postRecoveryMutationObserved"),
                    Is.EqualTo(true));
            }
        }

        [Test]
        public void EvidenceLootMovementUsesExistingMovementCadence()
        {
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var load = ActiveLoad();
                var lobby = new LobbyRoomReadModel();
                lobby.Apply(new LobbyEntrySnapshot(
                    1,
                    2,
                    "neo",
                    Array.Empty<RoomSummary>()));
                ArenaClientRuntime runtime = CreateArenaRuntime(load, socket);
                var outbound = (RudpReliableOutbound)GetField(
                    runtime.Movement,
                    "reliableOutbound");
                SetField(outbound, "transportEpoch", (uint)1);
                runtime.Movement.ReadModel.Apply(new RudpStateSnapshot(
                    9,
                    1,
                    20,
                    new[] { new RudpSnapshotPlayer(1, 0, 0) }));
                runtime.Loot.Apply(new RudpDropStateSnapshot(
                    9,
                    1,
                    RudpLootResolutionState.Open,
                    new[]
                    {
                        new RudpLootDropProjection(
                            1,
                            2,
                            1,
                            1000,
                            0,
                            RudpLootDropState.Available,
                            0)
                    }));
                var binding = new ArenaInputBinding(
                    runtime.Movement,
                    runtime.Movement.ReadModel,
                    runtime.Combat,
                    runtime.Loot,
                    runtime.Presentation,
                    runtime.Input,
                    1);
                var driver = new DevelopmentPlayerFlowDriver(
                    "host",
                    lobby,
                    new StubRoomCommands(),
                    new StubHostStart(),
                    load,
                    new BattleResultReadModel(),
                    () => binding,
                    new StubCollectionApi(),
                    new CollectionReadModel(),
                    CancellationToken.None);
                SetField(driver, "activeBattleId", 9UL);
                SetField(driver, "targetDropId", 1UL);
                const double now = 10.0;

                Invoke(driver, "TickLoot", binding, now);
                Assert.That(((Task)GetField(driver, "operation")).Wait(1000), Is.True);
                Assert.That(Invoke(driver, "ObserveOperation"), Is.EqualTo(true));
                uint afterFirst = (uint)GetField(outbound, "nextSequence");

                Invoke(driver, "TickLoot", binding, now + 0.01);
                Assert.That(
                    (uint)GetField(outbound, "nextSequence"),
                    Is.EqualTo(afterFirst));
                Assert.That(GetField(driver, "operation"), Is.Null);

                Invoke(driver, "TickLoot", binding, now + 0.101);
                Assert.That(
                    (uint)GetField(outbound, "nextSequence"),
                    Is.EqualTo(afterFirst + 1));
            }
        }

        [Test]
        public void CrashEvidenceReopensOneBattleWithThirtySecondCollectionDeadline()
        {
            var load = ActiveLoad();
            var lobby = new LobbyRoomReadModel();
            lobby.Apply(new LobbyEntrySnapshot(
                1,
                2,
                "neo",
                Array.Empty<RoomSummary>()));
            var room = new RoomDetailProjection(
                7,
                "room",
                2,
                1,
                2,
                new[]
                {
                    new RoomMember(1, 2, "neo", false),
                    new RoomMember(2, 3, "trinity", false)
                });
            lobby.Apply(room);
            var result = new BattleResultReadModel();
            result.BeginBattle(7, 9);
            result.Apply(new BattleFinalResult(
                7,
                9,
                FinalResultOutcome.MonsterDefeated,
                new[]
                {
                    new FinalResultEntry(
                        1,
                        "neo",
                        FinalResultExitStatus.TerminalPresent,
                        300,
                        1,
                        true),
                    new FinalResultEntry(
                        2,
                        "trinity",
                        FinalResultExitStatus.TerminalPresent,
                        100,
                        2,
                        false)
                }));
            result.Apply(room);
            load.ApplyFinalResult(7, 9);
            var driver = new DevelopmentPlayerFlowDriver(
                "host",
                lobby,
                new StubRoomCommands(),
                new StubHostStart(),
                load,
                result,
                new Func<ArenaInputBinding>(() => null),
                new StubCollectionApi(),
                new CollectionReadModel(),
                CancellationToken.None,
                true,
                () => 2);
            SetField(driver, "activeBattleId", 9UL);
            SetField(driver, "lootObserved", true);
            double now = Time.realtimeSinceStartupAsDouble;
            LogAssert.Expect(
                LogType.Log,
                "[CrashContinuityEvidence] event=final_result " +
                    "role=host session=1 room=7 battle=9 generation=2 tick=0 " +
                    "x=0 y=0 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=3 remaining_s=0");
            Invoke(driver, "TickFinalResult", now);

            Assert.That(GetField(driver, "completedCycles"), Is.EqualTo(1));
            Assert.That(GetField(driver, "activeBattleId"), Is.EqualTo(0UL));
            Assert.That(
                (double)GetField(driver, "collectionDeadline") - now,
                Is.EqualTo(30.0).Within(0.1));
            Assert.That(GetField(driver, "crashEvidencePhase"), Is.EqualTo(3));

            foreach (string eventName in new[]
                     { "collection_pending", "collection_applied", "complete" })
            {
                LogAssert.Expect(
                    LogType.Log,
                    "[CrashContinuityEvidence] event=" + eventName +
                    " role=host session=1 room=7 battle=9 generation=2 tick=0 " +
                    "x=0 y=0 monster_hp=0 monster_max=0 drops=0 " +
                    "phase=3 remaining_s=0");
                Invoke(
                    driver,
                    "CrashEvidence",
                    eventName,
                    0UL,
                    0UL,
                    0UL,
                    0UL,
                    null,
                    null,
                    null);
            }

            driver.Tick();

            Assert.That(GetField(driver, "activeBattleId"), Is.EqualTo(0UL));
        }

        [Test]
        public void CrashEvidenceRetriesTransientCollectionFailureBeforeDeadline()
        {
            var driver = CreateCrashDriver(ActiveLoad(), () => 2);
            double now = Time.realtimeSinceStartupAsDouble;
            SetField(driver, "collectionPhase", 1);
            SetField(driver, "collectionDeadline", now + 30.0);
            SetField(
                driver,
                "collectionPoll",
                Task.FromException<CollectionSnapshot>(
                    new InvalidOperationException("transient")));

            LogAssert.ignoreFailingMessages = true;
            bool observed;
            try
            {
                observed = (bool)Invoke(driver, "ObserveCollectionPoll");
            }
            finally
            {
                LogAssert.ignoreFailingMessages = false;
            }

            Assert.That(observed, Is.EqualTo(true));
            Assert.That(GetField(driver, "collectionPoll"), Is.Null);
            Assert.That(GetField(driver, "finished"), Is.EqualTo(false));
            Assert.That(
                (double)GetField(driver, "nextCollectionPollAt"),
                Is.GreaterThan(now));
        }

        [Test]
        public void ResumeApplierRequiresAuthenticatedCurrentGeneration()
        {
            var load = new BattleLoadReadModel();
            load.Apply(new ArenaLoadEntry(7, 9));
            load.Apply(new ArenaGameplayStart(
                7,
                9,
                new[]
                {
                    new BattleParticipant(1, 2, "neo"),
                    new BattleParticipant(2, 3, "trinity")
                }));
            var result = new BattleResultReadModel();
            result.BeginBattle(7, 9);
            var session = new PlayerSessionReadModel();
            var snapshot = new BattleResumeSnapshot(
                1,
                8,
                7,
                9,
                1,
                3,
                BattleResumePhase.Combat,
                1000,
                4,
                new[]
                {
                    new BattleResumePlayerState(1, 10, 20, true, 90, 100, true),
                    new BattleResumePlayerState(2, 30, 40, true, 80, 100, true)
                },
                null,
                Array.Empty<BattleResumeDropState>(),
                9,
                null);

            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var runtime = CreateArenaRuntime(load, socket);
                var applier = new BattleSessionResumeApplier(
                    load,
                    result,
                    runtime,
                    session);

                Assert.That(applier.Apply(snapshot), Is.False);

                session.BeginAuthentication();
                session.Apply(new WelcomeSession(1, 1, 2, 0, "neo"));
                Assert.That(applier.Apply(snapshot), Is.False);
                Assert.That(session.SessionGeneration, Is.EqualTo(2));
            }
        }

        [Test]
        public void ResumeSnapshotStartsFreshRudpSnapshotSequenceSpace()
        {
            var load = ActiveLoad();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                ArenaClientRuntime runtime = CreateArenaRuntime(load, socket);
                var snapshot = new BattleResumeSnapshot(
                    1,
                    100,
                    7,
                    9,
                    1,
                    2,
                    BattleResumePhase.Combat,
                    30000,
                    4,
                    new[]
                    {
                        new BattleResumePlayerState(1, 10, 20, false, 0, 0, true),
                        new BattleResumePlayerState(2, 30, 40, false, 0, 0, true)
                    },
                    new BattleResumeMonsterState(
                        1,
                        0,
                        0,
                        1500,
                        1600,
                        BattleResumeMonsterState.Alive),
                    Array.Empty<BattleResumeDropState>(),
                    0,
                    null);

                Assert.That(runtime.ApplyResumeSnapshot(snapshot), Is.True);
                Assert.That(runtime.Movement.ReadModel.Apply(new RudpStateSnapshot(
                    9,
                    1,
                    5,
                    new[]
                    {
                        new RudpSnapshotPlayer(1, 250, 20),
                        new RudpSnapshotPlayer(2, 30, 40)
                    })), Is.True);
                Assert.That(runtime.Combat.Apply(new RudpMonsterStateSnapshot(
                    9,
                    1,
                    5,
                    1,
                    1400,
                    RudpMonsterState.Alive)), Is.True);
                Assert.That(runtime.Loot.Apply(new RudpDropStateSnapshot(
                    9,
                    1,
                    RudpLootResolutionState.Open,
                    new[]
                    {
                        new RudpLootDropProjection(
                            1,
                            2,
                            1,
                            100,
                            200,
                            RudpLootDropState.Available,
                            0)
                    })), Is.True);
                Assert.That(
                    runtime.Movement.ReadModel.Positions[1].PositionXMillimeters,
                    Is.EqualTo(250));
                Assert.That(runtime.Combat.HitPoints, Is.EqualTo(1400));
                Assert.That(
                    runtime.Loot.ResolutionState,
                    Is.EqualTo(RudpLootResolutionState.Open));
            }
        }

        [Test]
        public void ArenaControlsAreDisabledWhileReconnectIsLocked()
        {
            var load = ActiveLoad();
            var presentation = new ArenaPlayerFlowReadModel(
                load,
                new BattleMovementReadModel(9),
                new BattleCombatReadModel(9),
                new BattleLootReadModel(9));

            Assert.That(presentation.Snapshot().ControlsEnabled, Is.True);

            load.SetReconnectLocked(true);

            Assert.That(presentation.Snapshot().ControlsEnabled, Is.False);
        }

        [Test]
        public void EvidenceDriverFailsOperationCompletedBeforeReconnectLock()
        {
            var load = ActiveLoad();
            var driver = CreateDriver(load);
            SetField(driver, "operation", Task.FromException(
                new ObjectDisposedException("old-rudp")));
            SetField(driver, "operationStage", "attack");
            load.SetReconnectLocked(true);
            Invoke(driver, "OnReconnectStarted");

            LogAssert.ignoreFailingMessages = true;
            bool observed;
            try
            {
                observed = (bool)Invoke(driver, "ObserveOperation");
            }
            finally
            {
                LogAssert.ignoreFailingMessages = false;
            }

            Assert.That(observed, Is.False);
            Assert.That(GetField(driver, "finished"), Is.True);
        }

        [Test]
        public void EvidenceDriverConsumesDisposedOperationLatchedAtReconnectStart()
        {
            var load = ActiveLoad();
            var driver = CreateDriver(load);
            var completion = new TaskCompletionSource<object>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            SetField(driver, "operation", completion.Task);
            SetField(driver, "operationStage", "attack");
            load.SetReconnectLocked(true);
            Invoke(driver, "OnReconnectStarted");

            load.SetReconnectLocked(false);
            completion.SetException(new ObjectDisposedException("old-rudp"));
            LogAssert.ignoreFailingMessages = true;
            bool observed;
            try
            {
                observed = (bool)Invoke(driver, "ObserveOperation");
            }
            finally
            {
                LogAssert.ignoreFailingMessages = false;
            }

            Assert.That(observed, Is.True);
            Assert.That(GetField(driver, "operation"), Is.Null);
            Assert.That(GetField(driver, "finished"), Is.False);
        }

        [Test]
        public void EvidenceDriverStillFailsLatchedUnexpectedArenaOperationFault()
        {
            var load = ActiveLoad();
            var driver = CreateDriver(load);
            var completion = new TaskCompletionSource<object>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            SetField(driver, "operation", completion.Task);
            SetField(driver, "operationStage", "attack");
            load.SetReconnectLocked(true);
            Invoke(driver, "OnReconnectStarted");

            load.SetReconnectLocked(false);
            completion.SetException(new InvalidOperationException("unexpected"));
            LogAssert.ignoreFailingMessages = true;
            bool observed;
            try
            {
                observed = (bool)Invoke(driver, "ObserveOperation");
            }
            finally
            {
                LogAssert.ignoreFailingMessages = false;
            }

            Assert.That(observed, Is.False);
            Assert.That(GetField(driver, "finished"), Is.True);
        }

        [Test]
        public void BootstrapNotifiesEvidenceDriverWhenReconnectStarts()
        {
            var root = new GameObject("Reconnect lifecycle notification test");
            var shutdown = new CancellationTokenSource();
            try
            {
                var load = ActiveLoad();
                var driver = CreateDriver(load);
                var completion = new TaskCompletionSource<object>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                SetField(driver, "operation", completion.Task);
                SetField(driver, "operationStage", "attack");

                var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                SetField(bootstrap, "shutdown", shutdown);
                SetField(bootstrap, "battleLoad", load);
                SetField(bootstrap, "evidenceDriver", driver);

                Invoke(bootstrap, "BeginReconnect");
                Task reconnect = (Task)GetField(bootstrap, "reconnect");
                _ = reconnect.Exception;
                load.SetReconnectLocked(false);
                completion.SetException(new ObjectDisposedException("old-rudp"));

                LogAssert.ignoreFailingMessages = true;
                bool observed;
                try
                {
                    observed = (bool)Invoke(driver, "ObserveOperation");
                }
                finally
                {
                    LogAssert.ignoreFailingMessages = false;
                }

                Assert.That(observed, Is.True);
                Assert.That(GetField(driver, "finished"), Is.False);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(root);
                shutdown.Dispose();
            }
        }

        [Test]
        public void BootstrapReportsSnapshotAppliedOncePerReconnectLifecycle()
        {
            var root = new GameObject("Snapshot applied evidence lifecycle test");
            var shutdown = new CancellationTokenSource();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                try
                {
                    var load = ActiveLoad();
                    var result = new BattleResultReadModel();
                    result.BeginBattle(7, 9);
                    var session = new PlayerSessionReadModel();
                    session.BeginAuthentication();
                    session.Apply(new WelcomeSession(1, 1, 2, 0, "neo"));
                    var lobby = new LobbyRoomReadModel();
                    lobby.Apply(new LobbyEntrySnapshot(
                        1,
                        2,
                        "neo",
                        Array.Empty<RoomSummary>()));
                    var driver = new DevelopmentPlayerFlowDriver(
                        "host",
                        lobby,
                        new StubRoomCommands(),
                        new StubHostStart(),
                        load,
                        new BattleResultReadModel(),
                        new Func<ArenaInputBinding>(() => null),
                        new StubCollectionApi(),
                        new CollectionReadModel(),
                        CancellationToken.None,
                        true,
                        () => 2);
                    SetField(driver, "preCrashObserved", true);
                    var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                    SetField(bootstrap, "shutdown", shutdown);
                    SetField(bootstrap, "battleLoad", load);
                    SetField(bootstrap, "battleResult", result);
                    SetField(bootstrap, "session", session);
                    SetField(bootstrap, "arenaRuntime", CreateArenaRuntime(load, socket));
                    SetField(bootstrap, "evidenceDriver", driver);

                    LogAssert.Expect(
                        LogType.Log,
                        "[CrashContinuityEvidence] event=reconnect_started " +
                        "role=host session=1 room=7 battle=9 generation=2 tick=0 " +
                        "x=0 y=0 monster_hp=0 monster_max=0 drops=0 " +
                        "phase=0 remaining_s=0");
                    Invoke(bootstrap, "BeginReconnect");
                    Task firstReconnect = (Task)GetField(bootstrap, "reconnect");
                    _ = firstReconnect.Exception;

                    BattleResumeSnapshot snapshot = ResumeResultSnapshot();
                    LogAssert.Expect(
                        LogType.Log,
                        "[CrashContinuityEvidence] event=snapshot_applied " +
                        "role=host session=1 room=7 battle=9 generation=2 tick=0 " +
                        "x=0 y=0 monster_hp=0 monster_max=0 drops=0 " +
                        "phase=0 remaining_s=0");
                    Assert.That(
                        Invoke(bootstrap, "ApplyResumeSnapshot", snapshot),
                        Is.EqualTo(true));
                    Assert.That(
                        GetField(bootstrap, "snapshotAppliedEvidenceReported"),
                        Is.EqualTo(true));

                    Assert.That(
                        Invoke(bootstrap, "ApplyResumeSnapshot", snapshot),
                        Is.EqualTo(true));
                    Assert.That(GetField(driver, "finished"), Is.EqualTo(false));

                    SetField(bootstrap, "reconnect", null);
                    SetField(driver, "reconnectStarted", false);
                    load.SetReconnectLocked(false);
                    LogAssert.Expect(
                        LogType.Log,
                        "[CrashContinuityEvidence] event=reconnect_started " +
                        "role=host session=1 room=7 battle=9 generation=2 tick=0 " +
                        "x=0 y=0 monster_hp=0 monster_max=0 drops=0 " +
                        "phase=0 remaining_s=0");
                    Invoke(bootstrap, "BeginReconnect");
                    Task secondReconnect = (Task)GetField(bootstrap, "reconnect");
                    _ = secondReconnect.Exception;
                    Assert.That(
                        GetField(bootstrap, "snapshotAppliedEvidenceReported"),
                        Is.EqualTo(false));

                    LogAssert.Expect(
                        LogType.Log,
                        "[CrashContinuityEvidence] event=snapshot_applied " +
                        "role=host session=1 room=7 battle=9 generation=2 tick=0 " +
                        "x=0 y=0 monster_hp=0 monster_max=0 drops=0 " +
                        "phase=0 remaining_s=0");
                    Assert.That(
                        Invoke(bootstrap, "ApplyResumeSnapshot", snapshot),
                        Is.EqualTo(true));
                    Assert.That(GetField(driver, "finished"), Is.EqualTo(false));
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(root);
                }
            }
            shutdown.Dispose();
        }

        [Test]
        public void BootstrapRetiresArenaAfterTerminalReconnectFailure()
        {
            var root = new GameObject("Reconnect terminal failure test");
            var shutdown = new CancellationTokenSource();
            var subscription = new RecordingDisposable();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                try
                {
                    var load = ActiveLoad();
                    ArenaClientRuntime runtime = CreateArenaRuntime(load, socket);
                    var outbound = (RudpReliableOutbound)GetField(
                        runtime.Movement,
                        "reliableOutbound");
                    SetField(outbound, "transportEpoch", (uint)1);
                    var binding = new ArenaInputBinding(
                        runtime.Movement,
                        runtime.Movement.ReadModel,
                        runtime.Combat,
                        runtime.Loot,
                        runtime.Presentation,
                        runtime.Input,
                        1);
                    Assert.That(runtime.Combat.Apply(new RudpMonsterSpawned(
                        new RudpEventId(1, 1),
                        9,
                        RudpEventStreamKind.CombatLifecycle,
                        1,
                        1,
                        0,
                        0,
                        1600,
                        4)), Is.True);
                    var driver = CreateDriver(load, () => binding);
                    SetField(driver, "activeBattleId", load.BattleInstanceId);
                    SetField(driver, "movementStage", 2);
                    SetField(driver, "nextAttackAt", 0d);
                    SetField(driver, "createRequested", true);

                    var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                    SetField(bootstrap, "shutdown", shutdown);
                    SetField(bootstrap, "battleLoad", load);
                    SetField(bootstrap, "arenaRuntime", runtime);
                    SetField(bootstrap, "arenaBinding", binding);
                    SetField(bootstrap, "arenaRudpSubscription", subscription);
                    SetField(
                        bootstrap,
                        "arenaTick",
                        Task.FromException(new ObjectDisposedException("old-rudp")));
                    SetField(
                        bootstrap,
                        "arenaBind",
                        Task.FromException(new ObjectDisposedException("old-rudp")));
                    SetField(bootstrap, "reconnectAwaitingRudp", true);
                    SetField(
                        bootstrap,
                        "reconnect",
                        Task.FromException<BattleSessionReconnectResult>(
                            new InvalidOperationException("resume failed")));

                    Invoke(bootstrap, "ObserveReconnect");
                    driver.Tick();

                    Assert.That(subscription.IsDisposed, Is.True);
                    Assert.That(GetField(bootstrap, "arenaRudpSubscription"), Is.Null);
                    Assert.That(GetField(bootstrap, "arenaBinding"), Is.Null);
                    Assert.That(GetField(bootstrap, "arenaRuntime"), Is.Null);
                    Assert.That(GetField(bootstrap, "arenaTick"), Is.Not.Null);
                    Assert.That(GetField(bootstrap, "arenaBind"), Is.Not.Null);
                    Assert.That(GetField(bootstrap, "reconnectAwaitingRudp"), Is.False);
                    Assert.That(load.IsGameplayActive, Is.False);
                    Assert.That(GetField(driver, "attackSubmissions"), Is.EqualTo(0));
                    Assert.That(GetField(driver, "operation"), Is.Null);

                    LogAssert.Expect(
                        LogType.Warning,
                        "Arena transport request failed safely.");
                    LogAssert.Expect(
                        LogType.Warning,
                        "Arena transport request failed safely.");
                    Invoke(bootstrap, "ObserveArenaTasks");
                    Assert.That(GetField(bootstrap, "arenaTick"), Is.Null);
                    Assert.That(GetField(bootstrap, "arenaBind"), Is.Null);
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(root);
                }
            }
            shutdown.Dispose();
        }

        [Test]
        public async Task BootstrapStopsOldTransportAfterTerminalReconnectFailure()
        {
            var root = new GameObject("Reconnect transport retirement test");
            var shutdown = new CancellationTokenSource();
            var lifetime = new PlayerFlowTransportLifetime();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                try
                {
                    var inbound = new RudpInboundPump(
                        socket,
                        new IPEndPoint(IPAddress.Loopback, 40000),
                        1,
                        2);
                    Task rudpTask = lifetime.StartRudp(inbound);
                    var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                    SetField(bootstrap, "shutdown", shutdown);
                    SetField(bootstrap, "battleLoad", ActiveLoad());
                    SetField(bootstrap, "transportLifetime", lifetime);
                    SetField(bootstrap, "udp", socket);
                    SetField(
                        bootstrap,
                        "reconnect",
                        Task.FromException<BattleSessionReconnectResult>(
                            new InvalidOperationException("resume unavailable")));

                    Invoke(bootstrap, "ObserveReconnect");

                    Assert.That(lifetime.IsRunning, Is.False);
                    await rudpTask;
                }
                finally
                {
                    await lifetime.StopAsync();
                    UnityEngine.Object.DestroyImmediate(root);
                    shutdown.Dispose();
                }
            }
        }

        [Test]
        public async Task BootstrapIgnoresOldReliableFailureWithoutCurrentRudpPump()
        {
            var root = new GameObject("Reconnect transport generation test");
            var shutdown = new CancellationTokenSource();
            var lifetime = new PlayerFlowTransportLifetime();
            try
            {
                var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                SetField(bootstrap, "shutdown", shutdown);
                SetField(bootstrap, "transportLifetime", lifetime);
                using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
                {
                    var inbound = new RudpInboundPump(
                        socket,
                        new IPEndPoint(IPAddress.Loopback, 40000),
                        1,
                        2);
                    var oldReliable = new RudpReliableOutbound(
                        new StubRudpSender(),
                        inbound);
                    SetField(oldReliable, "confirmedFailure", true);
                    SetField(bootstrap, "reliableOutbound", oldReliable);

                    Invoke(bootstrap, "ObserveTransport");

                    Assert.That(GetField(bootstrap, "rudpFailureHandled"), Is.False);
                    Assert.That(GetField(lifetime, "failurePosted"), Is.False);
                }
            }
            finally
            {
                await lifetime.StopAsync();
                UnityEngine.Object.DestroyImmediate(root);
                shutdown.Dispose();
            }
        }

        [Test]
        public async Task BootstrapObservesTransportStopFailureSafely()
        {
            var lifetime = new PlayerFlowTransportLifetime();
            try
            {
                SetField(
                    lifetime,
                    "tcpTask",
                    Task.FromException(new ObjectDisposedException("old-transport")));
                MethodInfo method = typeof(ProductPlayerFlowBootstrap).GetMethod(
                    "StopTransportSafelyAsync",
                    BindingFlags.Static | BindingFlags.NonPublic);
                Assert.That(method, Is.Not.Null);

                LogAssert.Expect(
                    LogType.Warning,
                    "Player transport shutdown failed safely.");
                await (Task)method.Invoke(
                    null,
                    new object[] { lifetime });
            }
            finally
            {
                SetField(lifetime, "tcpTask", Task.CompletedTask);
                await lifetime.StopAsync();
            }
        }

        [Test]
        public async Task BootstrapKeepsExistingRoomCommandsOnResumedTcpSender()
        {
            var root = new GameObject("Reconnect TCP sender test");
            try
            {
                var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                var initial = new RecordingTcpSender();
                var resumed = new RecordingTcpSender();
                ITcpCommandSender stable = (ITcpCommandSender)Invoke(
                    bootstrap,
                    "BindTcpSender",
                    initial);
                var correlator = new RoomCommandCorrelator();
                var commands = new LobbyRoomCommandCoordinator(stable, correlator);

                Task<RoomCommandResult> first = commands.SetReadyAsync(
                    true,
                    CancellationToken.None);
                correlator.Complete(1, RoomCommandResult.Ok);
                await first;

                ITcpCommandSender rebound = (ITcpCommandSender)Invoke(
                    bootstrap,
                    "BindTcpSender",
                    resumed);
                Task<RoomCommandResult> second = commands.SetReadyAsync(
                    true,
                    CancellationToken.None);
                correlator.Complete(2, RoomCommandResult.Ok);
                await second;

                Assert.That(rebound, Is.SameAs(stable));
                Assert.That(initial.Frames, Has.Count.EqualTo(1));
                Assert.That(resumed.Frames, Has.Count.EqualTo(1));
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void EvidenceDriverStillFailsUnexpectedArenaOperationFault()
        {
            var load = ActiveLoad();
            var driver = CreateDriver(load);
            SetField(driver, "operation", Task.FromException(
                new InvalidOperationException("unexpected")));
            SetField(driver, "operationStage", "attack");
            load.SetReconnectLocked(true);

            LogAssert.ignoreFailingMessages = true;
            bool observed;
            try
            {
                observed = (bool)Invoke(driver, "ObserveOperation");
            }
            finally
            {
                LogAssert.ignoreFailingMessages = false;
            }

            Assert.That(observed, Is.False);
            Assert.That(GetField(driver, "finished"), Is.True);
        }

        [Test]
        public void EvidenceDriverDoesNotStartArenaFlowWhileReconnectIsLocked()
        {
            var load = ActiveLoad();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                ArenaClientRuntime runtime = CreateArenaRuntime(load, socket);
                var outbound = (RudpReliableOutbound)GetField(
                    runtime.Movement,
                    "reliableOutbound");
                SetField(outbound, "transportEpoch", (uint)1);
                var binding = new ArenaInputBinding(
                    runtime.Movement,
                    runtime.Movement.ReadModel,
                    runtime.Combat,
                    runtime.Loot,
                    runtime.Presentation,
                    runtime.Input,
                    1);
                var driver = CreateDriver(load, () => binding);
                load.SetReconnectLocked(true);

                driver.Tick();

                Assert.That(GetField(driver, "activeBattleId"), Is.EqualTo(0UL));
            }
        }

        [Test]
        public void BootstrapDoesNotRestartOldArenaTickWhileReconnectIsPending()
        {
            var root = new GameObject("Reconnect arena tick test");
            var shutdown = new CancellationTokenSource();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                try
                {
                    var load = ActiveLoad();
                    var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                    SetField(bootstrap, "shutdown", shutdown);
                    SetField(bootstrap, "battleLoad", load);
                    SetField(bootstrap, "arenaRuntime", CreateArenaRuntime(load, socket));
                    SetField(bootstrap, "reconnect",
                        new TaskCompletionSource<BattleSessionReconnectResult>().Task);

                    Invoke(bootstrap, "Update");

                    Assert.That(GetField(bootstrap, "arenaTick"), Is.Null);
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(root);
                }
            }
            shutdown.Dispose();
        }

        [Test]
        public void BootstrapStartsArenaTickAfterReconnectHasResumed()
        {
            var root = new GameObject("Resumed arena tick test");
            var shutdown = new CancellationTokenSource();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                try
                {
                    var load = ActiveLoad();
                    var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                    SetField(bootstrap, "shutdown", shutdown);
                    SetField(bootstrap, "battleLoad", load);
                    SetField(bootstrap, "arenaRuntime", CreateArenaRuntime(load, socket));

                    Invoke(bootstrap, "Update");

                    Assert.That(GetField(bootstrap, "arenaTick"), Is.Not.Null);
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(root);
                }
            }
            shutdown.Dispose();
        }

        private static BattleLoadReadModel ActiveLoad()
        {
            var load = new BattleLoadReadModel();
            load.Apply(new ArenaLoadEntry(7, 9));
            load.Apply(new ArenaGameplayStart(
                7,
                9,
                new[] { new BattleParticipant(1, 2, "neo") }));
            return load;
        }

        private static BattleResumeSnapshot ResumeResultSnapshot()
        {
            return new BattleResumeSnapshot(
                1,
                8,
                7,
                9,
                1,
                2,
                BattleResumePhase.Result,
                0,
                4,
                new[]
                {
                    new BattleResumePlayerState(1, 10, 20, true, 90, 100, true),
                    new BattleResumePlayerState(2, 30, 40, true, 80, 100, true)
                },
                null,
                Array.Empty<BattleResumeDropState>(),
                9,
                new BattleResumeResultState(
                    FinalResultOutcome.MonsterDefeated,
                    new[]
                    {
                        new FinalResultEntry(
                            1,
                            "neo",
                            FinalResultExitStatus.TerminalPresent,
                            90,
                            1,
                            true),
                        new FinalResultEntry(
                            2,
                            "trinity",
                            FinalResultExitStatus.TerminalExited,
                            80,
                            2,
                            false)
                    }));
        }

        private static DevelopmentPlayerFlowDriver CreateDriver(
            BattleLoadReadModel load)
        {
            return CreateDriver(load, () => null);
        }

        private static DevelopmentPlayerFlowDriver CreateDriver(
            BattleLoadReadModel load,
            Func<ArenaInputBinding> arena)
        {
            return new DevelopmentPlayerFlowDriver(
                "host",
                new LobbyRoomReadModel(),
                new StubRoomCommands(),
                new StubHostStart(),
                load,
                new BattleResultReadModel(),
                arena,
                new StubCollectionApi(),
                new CollectionReadModel(),
                CancellationToken.None);
        }

        private static DevelopmentPlayerFlowDriver CreateCrashDriver(
            BattleLoadReadModel load,
            Func<ulong> sessionGeneration)
        {
            return new DevelopmentPlayerFlowDriver(
                "host",
                new LobbyRoomReadModel(),
                new StubRoomCommands(),
                new StubHostStart(),
                load,
                new BattleResultReadModel(),
                new Func<ArenaInputBinding>(() => null),
                new StubCollectionApi(),
                new CollectionReadModel(),
                CancellationToken.None,
                true,
                sessionGeneration);
        }

        private static ArenaClientRuntime CreateArenaRuntime(
            BattleLoadReadModel load,
            UdpClient socket)
        {
            var inbound = new RudpInboundPump(
                socket,
                new IPEndPoint(IPAddress.Loopback, 40000),
                1,
                2);
            return new ArenaClientRuntime(
                new StubTcpSender(),
                new StubRudpSender(),
                inbound,
                load,
                1,
                2,
                9);
        }

        private static object GetField(object target, string name)
        {
            FieldInfo field = target.GetType().GetField(
                name,
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null);
            return field.GetValue(target);
        }

        private static void SetField(object target, string name, object value)
        {
            FieldInfo field = target.GetType().GetField(
                name,
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null);
            field.SetValue(target, value);
        }

        private static object Invoke(object target, string name, params object[] arguments)
        {
            MethodInfo method = target.GetType().GetMethod(
                name,
                BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            Assert.That(method, Is.Not.Null);
            return method.Invoke(target, arguments);
        }

        private sealed class StubRoomCommands : ILobbyRoomCommands
        {
            public Task<RoomCommandResult> CreateAsync(
                string title,
                byte capacity,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }

            public Task<RoomCommandResult> JoinAsync(
                ulong roomId,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }

            public Task<RoomCommandResult> LeaveAsync(
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }

            public Task<RoomCommandResult> SetReadyAsync(
                bool ready,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }

            public Task<RoomCommandResult> KickAsync(
                ulong targetSessionId,
                ulong targetSessionGeneration,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }
        }

        private sealed class StubHostStart : IRoomHostStartAction
        {
            public Task<RoomCommandResult> StartAsync(
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }
        }

        private sealed class StubCollectionApi : ICollectionApi
        {
            public Task<CollectionSnapshot> FetchAsync(
                CancellationToken cancellationToken)
            {
                return Task.FromResult(new CollectionSnapshot(
                    Array.Empty<CollectionItem>(),
                    0,
                    0));
            }
        }

        private sealed class StubTcpSender : ITcpCommandSender
        {
            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                return Task.CompletedTask;
            }
        }

        private sealed class RecordingTcpSender : ITcpCommandSender
        {
            public List<byte[]> Frames { get; } = new List<byte[]>();

            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                Frames.Add(frame);
                return Task.CompletedTask;
            }
        }

        private sealed class StubRudpSender : IRudpDatagramSender
        {
            public Task SendAsync(
                byte[] datagram,
                CancellationToken cancellationToken)
            {
                return Task.CompletedTask;
            }
        }

        private sealed class RecordingDisposable : IDisposable
        {
            public bool IsDisposed { get; private set; }

            public void Dispose()
            {
                IsDisposed = true;
            }
        }
    }
}
