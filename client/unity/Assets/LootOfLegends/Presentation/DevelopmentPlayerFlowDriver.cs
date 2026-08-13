using System;
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
        private const string EvidenceRoomTitle = "Portfolio Evidence";
        private const double AttackIntervalSeconds = 0.751;
        private const double MovementRetryIntervalSeconds = 0.1;
        private const double FinalLootProjectionGraceSeconds = 2.0;
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
            startedAt = Time.realtimeSinceStartupAsDouble;
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = 240;
            Evidence("authenticated");
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
            ArenaInputBinding binding = arena();
            if (battleLoad.IsGameplayActive && binding != null &&
                binding.IsTransportReady)
            {
                if (activeBattleId != battleLoad.BattleInstanceId)
                {
                    BeginBattle(binding);
                }
                TickActiveBattle(binding, now);
                return;
            }
            if (activeBattleId != 0 && battleResult.HasFinalResult)
            {
                TickFinalResult(now);
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
            Evidence("arena_gameplay", completedCycles + 1, activeBattleId);
        }

        private void TickActiveBattle(ArenaInputBinding binding, double now)
        {
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
                if (completedCycles == 0 && collectionPhase == 0)
                {
                    collectionPhase = 1;
                    nextCollectionPollAt = now;
                    collectionDeadline = now + 10.0;
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
                nextCollectionPollAt = now + 0.25;
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
            }
            else if (collectionPhase == 2 &&
                     snapshot.PendingSettlementCount == 0 &&
                     snapshot.Items.Count > 0)
            {
                collectionPhase = 3;
                Evidence("collection_applied");
                if (completedCycles >= 2)
                {
                    Complete();
                }
            }
        }

        private bool ObserveOperation()
        {
            if (operation == null || !operation.IsCompleted)
            {
                return operation == null;
            }
            if (operation.IsFaulted || operation.IsCanceled)
            {
                string failure = operation.IsCanceled
                    ? "Canceled"
                    : operation.Exception?.GetBaseException().GetType().Name ??
                      "UnknownException";
                Fail(operationStage + "_" + failure);
                return false;
            }
            operation = null;
            operationStage = string.Empty;
            return true;
        }

        private bool ObserveCollectionPoll()
        {
            if (collectionPoll == null || !collectionPoll.IsCompleted)
            {
                return collectionPoll == null;
            }
            if (collectionPoll.IsFaulted || collectionPoll.IsCanceled)
            {
                Fail("collection_request");
                return false;
            }
            collectionPoll = null;
            return true;
        }

        private void Begin(Task task, string stage)
        {
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
            finished = true;
            Debug.LogError(
                "[PortfolioEvidence] role=" + role + " event=failure stage=" + stage);
            Application.Quit(2);
        }

        private void Evidence(
            string eventName,
            int cycle = 0,
            ulong battleId = 0,
            string detail = null)
        {
            string line = "[PortfolioEvidence] role=" + role + " event=" + eventName;
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
