using System;
using System.Globalization;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.Room;
using UnityEngine;

namespace LootOfLegends.Presentation
{
#if DEVELOPMENT_BUILD || UNITY_EDITOR
    public sealed class DevelopmentPlayerFlowDriver
    {
        private const string EvidenceRoomTitle = "Slice 8 Evidence";
        private const double AttackIntervalSeconds = 0.751;
        private const double MovementRetryIntervalSeconds = 0.1;
        private const double FinalLootProjectionGraceSeconds = 2.0;
        private const double CollectionPollIntervalSeconds = 0.25;
        private const double NormalCollectionDeadlineSeconds = 10.0;
        private const double CrashCollectionDeadlineSeconds = 30.0;
        private const double MaximumRunSeconds = 150.0;
        private readonly string role;
        private readonly bool isHost;
        private readonly LobbyRoomReadModel lobbyRoom;
        private readonly ILobbyRoomCommands roomCommands;
        private readonly IRoomHostStartAction hostStart;
        private readonly BattleLoadReadModel battleLoad;
        private readonly BattleResultReadModel battleResult;
        private readonly Func<ArenaInputBinding> arena;
        private readonly ICollectionApi collectionApi;
        private readonly CollectionReadModel collection;
        private readonly CancellationToken cancellationToken;
        private readonly bool crashContinuityEvidence;
        private readonly Func<ulong> sessionGeneration;
        private readonly double startedAt;
        private Task operation;
        private string operationStage = string.Empty;
        private Task collectionPoll;
        private int completedCycles;
        private int readyRequestedCycle;
        private int startRequestedCycle;
        private bool createRequested;
        private bool joinRequested;
        private bool roomObserved;
        private long observedRoomRevision;
        private ulong observedLoadBattleId;
        private int observedLoadState = -1;
        private ulong activeBattleId;
        private int movementStage;
        private int initialX;
        private int initialY;
        private double nextMovementSubmitAt;
        private int attackSubmissions;
        private double nextAttackAt;
        private ulong targetDropId;
        private bool movementStoppedForLoot;
        private double claimReadyAt;
        private int claimAttempts;
        private double lastClaimAt;
        private bool lootObserved;
        private bool finalObserved;
        private double finalResultSeenAt;
        private int collectionPhase;
        private double nextCollectionPollAt;
        private double collectionDeadline;
        private bool finished;
        private bool operationReconnectAffected;
        private bool preCrashObserved;
        private ulong preRestartSessionId;
        private ulong preRestartRoomId;
        private ulong preRestartBattleId;
        private ulong preRestartSessionGeneration;
        private bool reconnectStarted;
        private bool snapshotAppliedObserved;
        private bool snapshotAcknowledgedObserved;
        private bool postReconnectObserved;
        private bool postRecoveryMovementSubmitted;
        private bool postRecoveryMutationObserved;
        private int postRecoveryInitialX;
        private int postRecoveryInitialY;
        private double nextPostRecoveryMovementAt;
        private int crashEvidencePhase;

        public DevelopmentPlayerFlowDriver(
            string role,
            LobbyRoomReadModel lobbyRoom,
            ILobbyRoomCommands roomCommands,
            IRoomHostStartAction hostStart,
            BattleLoadReadModel battleLoad,
            BattleResultReadModel battleResult,
            Func<ArenaInputBinding> arena,
            ICollectionApi collectionApi,
            CollectionReadModel collection,
            CancellationToken cancellationToken)
            : this(
                role,
                lobbyRoom,
                roomCommands,
                hostStart,
                battleLoad,
                battleResult,
                arena,
                collectionApi,
                collection,
                cancellationToken,
                false,
                () => 0)
        {
        }

        public DevelopmentPlayerFlowDriver(
            string role,
            LobbyRoomReadModel lobbyRoom,
            ILobbyRoomCommands roomCommands,
            IRoomHostStartAction hostStart,
            BattleLoadReadModel battleLoad,
            BattleResultReadModel battleResult,
            Func<ArenaInputBinding> arena,
            ICollectionApi collectionApi,
            CollectionReadModel collection,
            CancellationToken cancellationToken,
            bool crashContinuityEvidence,
            Func<ulong> sessionGeneration)
        {
            if (role != "host" && role != "join")
            {
                throw new ArgumentException("Unknown evidence role", nameof(role));
            }
            this.role = role;
            isHost = role == "host";
            this.lobbyRoom = lobbyRoom ??
                throw new ArgumentNullException(nameof(lobbyRoom));
            this.roomCommands = roomCommands ??
                throw new ArgumentNullException(nameof(roomCommands));
            this.hostStart = hostStart ??
                throw new ArgumentNullException(nameof(hostStart));
            this.battleLoad = battleLoad ??
                throw new ArgumentNullException(nameof(battleLoad));
            this.battleResult = battleResult ??
                throw new ArgumentNullException(nameof(battleResult));
            this.arena = arena ?? throw new ArgumentNullException(nameof(arena));
            this.collectionApi = collectionApi ??
                throw new ArgumentNullException(nameof(collectionApi));
            this.collection = collection ??
                throw new ArgumentNullException(nameof(collection));
            this.cancellationToken = cancellationToken;
            this.crashContinuityEvidence = crashContinuityEvidence;
            this.sessionGeneration = sessionGeneration ??
                throw new ArgumentNullException(nameof(sessionGeneration));
            startedAt = Time.realtimeSinceStartupAsDouble;
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = 240;
            Evidence("authenticated");
        }

        public void OnReconnectStarted()
        {
            if (operation != null && !operation.IsCompleted &&
                IsArenaOperationStage(operationStage))
            {
                operationReconnectAffected = true;
            }
            if (!crashContinuityEvidence || finished)
            {
                return;
            }
            if (reconnectStarted)
            {
                Fail("duplicate_reconnect");
                return;
            }
            if (!preCrashObserved)
            {
                Fail("reconnect_before_pre_crash");
                return;
            }
            reconnectStarted = true;
            snapshotAppliedObserved = false;
            snapshotAcknowledgedObserved = false;
            preRestartSessionId = lobbyRoom.SessionId;
            preRestartRoomId = battleLoad.RoomId;
            preRestartBattleId = battleLoad.BattleInstanceId;
            preRestartSessionGeneration = CurrentSessionGeneration();
            if (preRestartSessionId == 0 || preRestartRoomId == 0 ||
                preRestartBattleId == 0 || preRestartSessionGeneration == 0)
            {
                Fail("reconnect_identity");
                return;
            }
            CrashEvidence(
                "reconnect_started",
                preRestartSessionId,
                preRestartRoomId,
                preRestartBattleId,
                preRestartSessionGeneration);
        }

        public void OnReconnectFailed()
        {
            if (crashContinuityEvidence && reconnectStarted)
            {
                Fail("reconnect_failed");
            }
        }

        public void OnSnapshotApplied()
        {
            if (!crashContinuityEvidence || finished)
            {
                return;
            }
            if (!reconnectStarted)
            {
                Fail("snapshot_before_reconnect");
                return;
            }
            if (snapshotAppliedObserved)
            {
                Fail("duplicate_snapshot_applied");
                return;
            }
            snapshotAppliedObserved = true;
            CrashEvidence("snapshot_applied");
        }

        public void OnSnapshotAcknowledged()
        {
            if (!crashContinuityEvidence || finished)
            {
                return;
            }
            if (!reconnectStarted)
            {
                Fail("snapshot_acknowledged_before_reconnect");
                return;
            }
            if (!snapshotAppliedObserved)
            {
                Fail("snapshot_acknowledged_before_apply");
                return;
            }
            if (snapshotAcknowledgedObserved)
            {
                Fail("duplicate_snapshot_acknowledged");
                return;
            }
            snapshotAcknowledgedObserved = true;
            CrashEvidence("snapshot_acknowledged");
        }

        public void Tick()
        {
            if (finished)
            {
                return;
            }
            double now = Time.realtimeSinceStartupAsDouble;
            if (now - startedAt > MaximumRunSeconds)
            {
                Fail("timeout");
                return;
            }
            ObserveBattleLoad();
            if (!ObserveOperation() || !ObserveCollectionPoll())
            {
                return;
            }

            TickCollection(now);
            if (finished)
            {
                return;
            }
            ArenaInputBinding binding = arena();
            if (crashContinuityEvidence && reconnectStarted &&
                !postReconnectObserved &&
                !TryObservePostReconnect(binding))
            {
                return;
            }
            if (battleLoad.IsGameplayActive && !battleLoad.IsReconnectLocked &&
                binding != null &&
                binding.IsTransportReady)
            {
                if (activeBattleId != battleLoad.BattleInstanceId)
                {
                    BeginBattle(binding);
                }
                if (crashContinuityEvidence && postReconnectObserved &&
                    !postRecoveryMutationObserved)
                {
                    TickPostRecoveryMutation(binding, now);
                    return;
                }
                TickActiveBattle(binding, now);
                return;
            }
            if (activeBattleId != 0 && battleResult.HasFinalResult)
            {
                TickFinalResult(now);
                return;
            }
            if (crashContinuityEvidence && completedCycles >= 1)
            {
                return;
            }
            TickLobbyAndRoom();
        }

        private void ObserveBattleLoad()
        {
            int state = battleLoad.IsGameplayActive ? 2 :
                battleLoad.IsWaiting ? 1 :
                battleLoad.HasLoadFailure ? 3 : 0;
            if (state == observedLoadState &&
                battleLoad.BattleInstanceId == observedLoadBattleId)
            {
                return;
            }
            observedLoadState = state;
            observedLoadBattleId = battleLoad.BattleInstanceId;
            Evidence(
                "battle_load_state",
                completedCycles + 1,
                battleLoad.BattleInstanceId,
                "state=" + state);
        }

        private void TickLobbyAndRoom()
        {
            int cycle = completedCycles + 1;
            if (!lobbyRoom.IsInRoom)
            {
                roomObserved = false;
                if (operation != null)
                {
                    return;
                }
                if (isHost && !createRequested)
                {
                    createRequested = true;
                    Begin(
                        RequireAccepted(roomCommands.CreateAsync(
                            EvidenceRoomTitle, 2, cancellationToken)),
                        "create_room");
                }
                else if (!isHost && !joinRequested)
                {
                    LobbyRoomSummaryView room = lobbyRoom.Lobby.Rooms.FirstOrDefault(
                        candidate => candidate.Title == EvidenceRoomTitle &&
                                     !candidate.IsFull);
                    if (room != null)
                    {
                        joinRequested = true;
                        Begin(
                            RequireAccepted(roomCommands.JoinAsync(
                                room.RoomId, cancellationToken)),
                            "join_room");
                    }
                }
                return;
            }

            if (!roomObserved)
            {
                roomObserved = true;
                Evidence("room_projection", cycle);
            }
            RoomPresentationSnapshot roomState = lobbyRoom.Room;
            if (roomState != null && roomState.Revision != observedRoomRevision)
            {
                observedRoomRevision = roomState.Revision;
                Evidence(
                    "room_state",
                    cycle,
                    0,
                    "members=" + roomState.Members.Count +
                    " ready=" + roomState.Members.Count(member => member.Ready) +
                    " can_start=" + (roomState.CanStart ? 1 : 0));
            }
            if (roomState == null || roomState.Members.Count != 2 || operation != null)
            {
                return;
            }
            RoomMemberPresentation local = roomState.Members.FirstOrDefault(
                member => member.IsLocal);
            if (local == null)
            {
                Fail("local_room_projection");
                return;
            }
            if (!local.Ready && readyRequestedCycle < cycle)
            {
                readyRequestedCycle = cycle;
                Begin(
                    RequireAccepted(roomCommands.SetReadyAsync(
                        true, cancellationToken)),
                    "ready");
                return;
            }
            if (isHost && roomState.CanStart && startRequestedCycle < cycle)
            {
                startRequestedCycle = cycle;
                Begin(
                    RequireAccepted(hostStart.StartAsync(cancellationToken)),
                    "host_start");
            }
        }

        private void BeginBattle(ArenaInputBinding binding)
        {
            activeBattleId = battleLoad.BattleInstanceId;
            movementStage = 0;
            attackSubmissions = 0;
            targetDropId = 0;
            movementStoppedForLoot = false;
            claimAttempts = 0;
            lootObserved = false;
            finalObserved = false;
            finalResultSeenAt = 0;
            nextMovementSubmitAt = 0;
            nextAttackAt = 0;
            if (!crashContinuityEvidence)
            {
                preCrashObserved = false;
                reconnectStarted = false;
                postReconnectObserved = false;
                postRecoveryMovementSubmitted = false;
                postRecoveryInitialX = 0;
                postRecoveryInitialY = 0;
                nextPostRecoveryMovementAt = 0;
                crashEvidencePhase = 0;
            }
            Evidence("arena_gameplay", completedCycles + 1, activeBattleId);
        }

        private void TickActiveBattle(ArenaInputBinding binding, double now)
        {
            if (crashContinuityEvidence && preCrashObserved &&
                !reconnectStarted)
            {
                return;
            }
            if (battleResult.HasFinalResult && !lootObserved)
            {
                Fail("final_before_loot");
                return;
            }
            if (binding.Loot.Drops.Count >= 2)
            {
                TickLoot(binding, now);
                return;
            }
            if (movementStage < 2)
            {
                TickMovementProof(binding, now);
                return;
            }
            TryEmitPreCrashState(binding);
            if (finished || (crashContinuityEvidence && preCrashObserved &&
                             !reconnectStarted))
            {
                return;
            }
            if (operation == null && binding.Combat.HasMonster &&
                now >= nextAttackAt)
            {
                attackSubmissions++;
                nextAttackAt = now + AttackIntervalSeconds;
                Begin(
                    binding.Input.AttackAsync(
                        binding.Combat.MonsterId, cancellationToken),
                    "attack");
            }
        }

        private bool TryObservePostReconnect(ArenaInputBinding binding)
        {
            if (binding == null || !battleLoad.IsGameplayActive ||
                battleLoad.IsReconnectLocked ||
                !binding.IsTransportReady ||
                !snapshotAcknowledgedObserved)
            {
                return false;
            }
            ulong currentSessionId = lobbyRoom.SessionId;
            ulong currentRoomId = battleLoad.RoomId;
            ulong currentBattleId = battleLoad.BattleInstanceId;
            if (currentSessionId == 0 || currentRoomId == 0 ||
                currentBattleId == 0 ||
                currentSessionId != preRestartSessionId ||
                currentRoomId != preRestartRoomId ||
                currentBattleId != preRestartBattleId)
            {
                Fail("reconnect_identity");
                return false;
            }
            ulong currentGeneration = CurrentSessionGeneration();
            if (finished)
            {
                return false;
            }
            if (currentGeneration <= preRestartSessionGeneration)
            {
                Fail("reconnect_generation");
                return false;
            }
            if (!binding.MovementReadModel.Positions.TryGetValue(
                    currentSessionId,
                    out PlayerPosition position))
            {
                return false;
            }
            ArenaPresentationSnapshot presentation = binding.Presentation.Snapshot();
            GetPresentationPhase(
                presentation,
                out int phase,
                out uint remainingSeconds);
            if (phase != 1 || remainingSeconds == 0)
            {
                return false;
            }
            postRecoveryInitialX = position.PositionXMillimeters;
            postRecoveryInitialY = position.PositionYMillimeters;
            postReconnectObserved = true;
            postRecoveryMovementSubmitted = false;
            postRecoveryMutationObserved = false;
            nextPostRecoveryMovementAt = 0;
            CrashEvidence(
                "post_reconnect_state",
                currentSessionId,
                currentRoomId,
                currentBattleId,
                currentGeneration,
                presentation: presentation);
            return true;
        }

        private void TickPostRecoveryMutation(
            ArenaInputBinding binding,
            double now)
        {
            if (!postReconnectObserved || postRecoveryMutationObserved ||
                operation != null)
            {
                return;
            }
            if (!binding.MovementReadModel.Positions.TryGetValue(
                    lobbyRoom.SessionId,
                    out PlayerPosition position))
            {
                return;
            }
            if (postRecoveryMovementSubmitted &&
                (position.PositionXMillimeters != postRecoveryInitialX ||
                 position.PositionYMillimeters != postRecoveryInitialY))
            {
                postRecoveryMutationObserved = true;
                CrashEvidence("post_recovery_mutation");
                nextAttackAt = now;
                Begin(
                    binding.Input.MoveAsync(0, 0, cancellationToken),
                    "post_recovery_movement_stop");
                return;
            }
            if (!postRecoveryMovementSubmitted)
            {
                postRecoveryInitialX = position.PositionXMillimeters;
                postRecoveryInitialY = position.PositionYMillimeters;
            }
            if (postRecoveryMovementSubmitted &&
                now < nextPostRecoveryMovementAt)
            {
                return;
            }
            postRecoveryMovementSubmitted = true;
            nextPostRecoveryMovementAt = now + MovementRetryIntervalSeconds;
            Begin(
                binding.Input.MoveAsync(
                    isHost ? short.MaxValue : (short)0,
                    isHost ? (short)0 : short.MaxValue,
                    cancellationToken),
                "post_recovery_movement");
        }

        private void TryEmitPreCrashState(ArenaInputBinding binding)
        {
            if (!crashContinuityEvidence || preCrashObserved || binding == null ||
                !binding.MovementReadModel.Positions.TryGetValue(
                    lobbyRoom.SessionId,
                    out PlayerPosition position) ||
                (position.PositionXMillimeters == initialX &&
                 position.PositionYMillimeters == initialY))
            {
                return;
            }
            if (!binding.Combat.HasMonster ||
                binding.Combat.HitPoints == 0 ||
                binding.Combat.MaximumHitPoints == 0 ||
                binding.Combat.HitPoints >= binding.Combat.MaximumHitPoints ||
                !string.IsNullOrEmpty(binding.Combat.OutcomeName))
            {
                return;
            }
            ArenaPresentationSnapshot presentation = binding.Presentation.Snapshot();
            GetPresentationPhase(
                presentation,
                out int phase,
                out uint remainingSeconds);
            if (phase != 1 || remainingSeconds == 0)
            {
                return;
            }
            preCrashObserved = true;
            CrashEvidence("pre_crash_state", presentation: presentation);
        }

        private void TickMovementProof(ArenaInputBinding binding, double now)
        {
            if (!binding.MovementReadModel.Positions.TryGetValue(
                    lobbyRoom.SessionId,
                    out PlayerPosition position) || operation != null)
            {
                return;
            }
            if (movementStage == 0)
            {
                initialX = position.PositionXMillimeters;
                initialY = position.PositionYMillimeters;
                movementStage = 1;
            }
            if (position.PositionXMillimeters != initialX ||
                position.PositionYMillimeters != initialY)
            {
                movementStage = 2;
                nextAttackAt = now;
                Begin(
                    binding.Input.MoveAsync(0, 0, cancellationToken),
                    "movement_stop");
                Evidence(
                    "movement_server_projection",
                    completedCycles + 1,
                    activeBattleId);
                return;
            }
            if (now < nextMovementSubmitAt)
            {
                return;
            }
            nextMovementSubmitAt = now + MovementRetryIntervalSeconds;
            Begin(
                binding.Input.MoveAsync(
                    isHost ? short.MaxValue : (short)0,
                    isHost ? (short)0 : short.MaxValue,
                    cancellationToken),
                "movement_submit");
        }

        private void TickLoot(ArenaInputBinding binding, double now)
        {
            if (targetDropId == 0)
            {
                BattleLootDropView selected = isHost
                    ? binding.Loot.Drops.OrderBy(drop => drop.DropId).First()
                    : binding.Loot.Drops.OrderByDescending(drop => drop.DropId).First();
                targetDropId = selected.DropId;
                Evidence(
                    "combat_server_terminal",
                    completedCycles + 1,
                    activeBattleId,
                    "attack_submissions=" + attackSubmissions);
            }
            BattleLootDropView target = binding.Loot.Drops.FirstOrDefault(
                drop => drop.DropId == targetDropId);
            if (target == null)
            {
                return;
            }
            if (target.OwnerSessionId == lobbyRoom.SessionId)
            {
                if (!lootObserved)
                {
                    lootObserved = true;
                    Evidence("loot_server_owner", completedCycles + 1, activeBattleId);
                }
                return;
            }
            if (!target.IsAvailable)
            {
                Fail("loot_owned_by_other");
                return;
            }
            if (!binding.MovementReadModel.Positions.TryGetValue(
                    lobbyRoom.SessionId,
                    out PlayerPosition position) || operation != null)
            {
                return;
            }
            long deltaX = (long)target.PositionXMillimeters -
                          position.PositionXMillimeters;
            long deltaY = (long)target.PositionYMillimeters -
                          position.PositionYMillimeters;
            long squaredDistance = deltaX * deltaX + deltaY * deltaY;
            if (squaredDistance > 250000)
            {
                movementStoppedForLoot = false;
                if (now < nextMovementSubmitAt)
                {
                    return;
                }
                nextMovementSubmitAt = now + MovementRetryIntervalSeconds;
                Begin(
                    binding.Input.MoveAsync(
                        Direction(deltaX), Direction(deltaY), cancellationToken),
                    "loot_movement");
                return;
            }
            if (!movementStoppedForLoot)
            {
                movementStoppedForLoot = true;
                claimReadyAt = now + 0.25;
                Begin(
                    binding.Input.MoveAsync(0, 0, cancellationToken),
                    "loot_stop");
                return;
            }
            if (now < claimReadyAt ||
                (claimAttempts > 0 && now - lastClaimAt < 1.0))
            {
                return;
            }
            if (claimAttempts >= 3)
            {
                Fail("loot_claim_not_confirmed");
                return;
            }
            claimAttempts++;
            lastClaimAt = now;
            Begin(
                binding.Input.ClaimAsync(targetDropId, cancellationToken),
                "loot_claim");
        }

        private void TickFinalResult(double now)
        {
            if (!lootObserved)
            {
                if (finalResultSeenAt == 0)
                {
                    finalResultSeenAt = now;
                }
                ArenaInputBinding binding = arena();
                BattleLootDropView target = binding?.Loot.Drops.FirstOrDefault(
                    drop => drop.DropId == targetDropId);
                if (target != null && target.OwnerSessionId == lobbyRoom.SessionId)
                {
                    lootObserved = true;
                    Evidence("loot_server_owner", completedCycles + 1, activeBattleId);
                }
                else if (now - finalResultSeenAt < FinalLootProjectionGraceSeconds)
                {
                    return;
                }
                else
                {
                    Fail("loot_projection_missing");
                    return;
                }
            }
            if (!finalObserved)
            {
                finalObserved = true;
                Evidence("final_result", completedCycles + 1, activeBattleId);
                CrashEvidence("final_result", phaseOverride: 3);
                if (completedCycles == 0 && collectionPhase == 0)
                {
                    collectionPhase = 1;
                    nextCollectionPollAt = now;
                    collectionDeadline = now +
                        (crashContinuityEvidence
                            ? CrashCollectionDeadlineSeconds
                            : NormalCollectionDeadlineSeconds);
                }
            }
            if (!battleResult.IsReadyForRematch || lobbyRoom.Room == null ||
                lobbyRoom.Room.Members.Any(member => member.Ready))
            {
                return;
            }
            completedCycles++;
            Evidence("room_reopened", completedCycles, activeBattleId);
            activeBattleId = 0;
            roomObserved = false;
            if (crashContinuityEvidence)
            {
                if (collectionPhase == 3)
                {
                    Complete();
                }
                return;
            }
            if (completedCycles >= 2)
            {
                if (collectionPhase == 3)
                {
                    Complete();
                }
                return;
            }
            TickLobbyAndRoom();
        }

        private void TickCollection(double now)
        {
            if (collectionPhase == 0 || collectionPhase == 3 ||
                collectionPoll != null)
            {
                return;
            }
            if (now > collectionDeadline)
            {
                Fail("collection_transition");
                return;
            }
            if (now >= nextCollectionPollAt)
            {
                nextCollectionPollAt = now + CollectionPollIntervalSeconds;
                collectionPoll = PollCollectionAsync();
            }
        }

        private async Task PollCollectionAsync()
        {
            collection.BeginRefresh();
            CollectionSnapshot snapshot = await collectionApi.FetchAsync(
                cancellationToken);
            collection.Apply(snapshot);
            if (collectionPhase == 1 && snapshot.PendingSettlementCount > 0 &&
                snapshot.Items.Count == 0)
            {
                collectionPhase = 2;
                Evidence("collection_pending");
                CrashEvidence("collection_pending");
            }
            else if (collectionPhase == 2 &&
                     snapshot.PendingSettlementCount == 0 &&
                     snapshot.Items.Count > 0)
            {
                collectionPhase = 3;
                Evidence("collection_applied");
                CrashEvidence("collection_applied");
                if (completedCycles >= (crashContinuityEvidence ? 1 : 2))
                {
                    Complete();
                }
            }
        }

        private bool ObserveOperation()
        {
            if (operation == null)
            {
                return true;
            }
            if (!operation.IsCompleted)
            {
                return false;
            }
            if (operation.IsFaulted || operation.IsCanceled)
            {
                if (IsExpectedReconnectArenaOperationFailure())
                {
                    operation = null;
                    operationStage = string.Empty;
                    operationReconnectAffected = false;
                    return true;
                }
                string failure = operation.IsCanceled
                    ? "Canceled"
                    : operation.Exception?.GetBaseException().GetType().Name ??
                      "UnknownException";
                Fail(operationStage + "_" + failure);
                return false;
            }
            operation = null;
            operationStage = string.Empty;
            operationReconnectAffected = false;
            return true;
        }

        private bool IsExpectedReconnectArenaOperationFailure()
        {
            if (!operationReconnectAffected || !IsArenaOperationStage(operationStage))
            {
                return false;
            }
            if (operation.IsCanceled)
            {
                return true;
            }
            Exception error = operation.Exception?.GetBaseException();
            return error is ObjectDisposedException || error is OperationCanceledException;
        }

        private static bool IsArenaOperationStage(string stage)
        {
            return stage == "movement_stop" || stage == "movement_submit" ||
                stage == "attack" || stage == "loot_movement" ||
                stage == "loot_stop" || stage == "loot_claim" ||
                stage == "post_recovery_movement" ||
                stage == "post_recovery_movement_stop";
        }

        private bool ObserveCollectionPoll()
        {
            if (collectionPoll == null || !collectionPoll.IsCompleted)
            {
                return collectionPoll == null;
            }
            if (collectionPoll.IsFaulted || collectionPoll.IsCanceled)
            {
                if (crashContinuityEvidence &&
                    Time.realtimeSinceStartupAsDouble <= collectionDeadline)
                {
                    collectionPoll = null;
                    nextCollectionPollAt =
                        Time.realtimeSinceStartupAsDouble +
                        CollectionPollIntervalSeconds;
                    return true;
                }
                Fail("collection_request");
                return false;
            }
            collectionPoll = null;
            return true;
        }

        private void Begin(Task task, string stage)
        {
            operationReconnectAffected = false;
            operation = task ?? throw new ArgumentNullException(nameof(task));
            operationStage = stage;
        }

        private static async Task RequireAccepted(Task<RoomCommandResult> operation)
        {
            if (await operation != RoomCommandResult.Ok)
            {
                throw new InvalidOperationException("Evidence command was rejected");
            }
        }

        private static short Direction(long delta)
        {
            if (delta > 0)
            {
                return short.MaxValue;
            }
            return delta < 0 ? (short)-short.MaxValue : (short)0;
        }

        private void Complete()
        {
            if (finished)
            {
                return;
            }
            CrashEvidence("complete");
            finished = true;
            Evidence("complete");
            Application.Quit(0);
        }

        private void Fail(string stage)
        {
            if (finished)
            {
                return;
            }
            CrashFailure(stage);
            finished = true;
            string loggedStage = crashContinuityEvidence
                ? SafeFailureStage(stage)
                : stage;
            Debug.LogError(
                "[Slice8Evidence] role=" + role +
                " event=failure stage=" + loggedStage);
            Application.Quit(2);
        }

        private ulong CurrentSessionGeneration()
        {
            try
            {
                return sessionGeneration();
            }
            catch (Exception)
            {
                return 0;
            }
        }

        private void CrashFailure(string stage)
        {
            if (!crashContinuityEvidence)
            {
                return;
            }
            CrashEvidence("failure", detail: "stage=" + SafeFailureStage(stage));
        }

        private static string SafeFailureStage(string stage)
        {
            return stage == "reconnect_identity" ||
                stage == "reconnect_generation" ||
                stage == "reconnect_failed" ||
                stage == "reconnect_before_pre_crash" ||
                stage == "duplicate_reconnect" ||
                stage == "snapshot_before_reconnect" ||
                stage == "duplicate_snapshot_applied" ||
                stage == "snapshot_acknowledged_before_reconnect" ||
                stage == "snapshot_acknowledged_before_apply" ||
                stage == "duplicate_snapshot_acknowledged" ||
                stage == "final_before_loot" ||
                stage == "loot_projection_missing" ||
                stage == "loot_owned_by_other" ||
                stage == "loot_claim_not_confirmed" ||
                stage == "collection_transition" ||
                stage == "collection_request" ||
                stage == "timeout"
                ? stage
                : "runtime";
        }

        private void CrashEvidence(
            string eventName,
            ulong sessionId = 0,
            ulong roomId = 0,
            ulong battleId = 0,
            ulong generation = 0,
            string detail = null,
            ArenaPresentationSnapshot presentation = null,
            int? phaseOverride = null)
        {
            if (!crashContinuityEvidence)
            {
                return;
            }
            ArenaInputBinding binding = arena();
            ulong currentSessionId = sessionId != 0
                ? sessionId
                : lobbyRoom.SessionId;
            ulong currentRoomId = roomId != 0 ? roomId : battleLoad.RoomId;
            ulong currentBattleId = battleId != 0
                ? battleId
                : battleLoad.BattleInstanceId != 0
                    ? battleLoad.BattleInstanceId
                    : activeBattleId;
            ulong currentGeneration = generation != 0
                ? generation
                : CurrentSessionGeneration();
            uint tick = 0;
            int positionX = 0;
            int positionY = 0;
            uint monsterHitPoints = 0;
            uint monsterMaximumHitPoints = 0;
            int dropCount = 0;
            if (binding != null)
            {
                tick = Math.Max(
                    binding.MovementReadModel.ServerTick,
                    binding.Combat.ServerTick);
                if (currentSessionId != 0 &&
                    binding.MovementReadModel.Positions.TryGetValue(
                        currentSessionId,
                        out PlayerPosition position))
                {
                    positionX = position.PositionXMillimeters;
                    positionY = position.PositionYMillimeters;
                }
                monsterHitPoints = binding.Combat.HitPoints;
                monsterMaximumHitPoints = binding.Combat.MaximumHitPoints;
                dropCount = binding.Loot.Drops.Count;
                presentation = presentation ?? binding.Presentation.Snapshot();
            }
            GetPresentationPhase(
                presentation,
                out int phase,
                out uint remainingSeconds);
            if (phaseOverride.HasValue)
            {
                phase = phaseOverride.Value;
                remainingSeconds = 0;
                crashEvidencePhase = phase;
            }
            else if (crashEvidencePhase == 3)
            {
                phase = 3;
                remainingSeconds = 0;
            }
            Debug.Log(
                "[CrashContinuityEvidence] event=" + eventName +
                " role=" + role +
                " session=" + Number(currentSessionId) +
                " room=" + Number(currentRoomId) +
                " battle=" + Number(currentBattleId) +
                " generation=" + Number(currentGeneration) +
                " tick=" + Number(tick) +
                " x=" + Number(positionX) +
                " y=" + Number(positionY) +
                " monster_hp=" + Number(monsterHitPoints) +
                " monster_max=" + Number(monsterMaximumHitPoints) +
                " drops=" + Number(dropCount) +
                " phase=" + Number(phase) +
                " remaining_s=" + Number(remainingSeconds) +
                (string.IsNullOrEmpty(detail) ? string.Empty : " " + detail));
        }

        private static void GetPresentationPhase(
            ArenaPresentationSnapshot presentation,
            out int phase,
            out uint remainingSeconds)
        {
            phase = 0;
            remainingSeconds = 0;
            if (presentation == null)
            {
                return;
            }
            if (presentation.Monster != null &&
                !string.IsNullOrEmpty(presentation.Monster.Outcome))
            {
                phase = 3;
                return;
            }
            if (presentation.RemainingLootSeconds > 0 ||
                presentation.CanClaimLoot)
            {
                phase = 2;
                remainingSeconds = presentation.RemainingLootSeconds;
                return;
            }
            if (presentation.RemainingCombatSeconds > 0)
            {
                phase = 1;
                remainingSeconds = presentation.RemainingCombatSeconds;
            }
        }

        private static string Number(ulong value)
        {
            return value.ToString(CultureInfo.InvariantCulture);
        }

        private static string Number(uint value)
        {
            return value.ToString(CultureInfo.InvariantCulture);
        }

        private static string Number(int value)
        {
            return value.ToString(CultureInfo.InvariantCulture);
        }

        private void Evidence(
            string eventName,
            int cycle = 0,
            ulong battleId = 0,
            string detail = null)
        {
            string line = "[Slice8Evidence] role=" + role + " event=" + eventName;
            if (cycle != 0)
            {
                line += " cycle=" + cycle;
            }
            if (battleId != 0)
            {
                line += " battle=" + battleId;
            }
            if (!string.IsNullOrEmpty(detail))
            {
                line += " " + detail;
            }
            Debug.Log(line);
        }
    }
#endif
}
