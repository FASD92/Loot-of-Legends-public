using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Battle.Movement;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Presentation.Room;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.Presentation
{
    public sealed class BattleHostStartAction : IRoomHostStartAction
    {
        private readonly BattleLoadCoordinator coordinator;

        public BattleHostStartAction(BattleLoadCoordinator coordinator)
        {
            this.coordinator = coordinator ??
                throw new ArgumentNullException(nameof(coordinator));
        }

        public async Task<RoomCommandResult> StartAsync(
            CancellationToken cancellationToken)
        {
            BattleCommandOutcome result = await coordinator.HostStartOutcomeAsync(
                cancellationToken);
            switch (result)
            {
                case BattleCommandOutcome.Ok:
                    return RoomCommandResult.Ok;
                case BattleCommandOutcome.RoomNotFound:
                    return RoomCommandResult.RoomNotFound;
                case BattleCommandOutcome.RoomNotOpen:
                    return RoomCommandResult.RoomClosed;
                case BattleCommandOutcome.NotInRoom:
                    return RoomCommandResult.NotInRoom;
                case BattleCommandOutcome.NotHost:
                    return RoomCommandResult.NotHost;
                case BattleCommandOutcome.NotEnoughPlayers:
                    return RoomCommandResult.NotEnoughPlayers;
                case BattleCommandOutcome.NotAllReady:
                    return RoomCommandResult.NotAllReady;
                case BattleCommandOutcome.StaleSession:
                    return RoomCommandResult.StaleSession;
                case BattleCommandOutcome.Overloaded:
                    return RoomCommandResult.RoomOverloaded;
                default:
                    return RoomCommandResult.InvalidArgument;
            }
        }
    }

    public sealed class ArenaInputBinding
    {
        public ArenaInputBinding(
            BattleMovementClient movement,
            BattleMovementReadModel movementReadModel,
            BattleCombatReadModel combat,
            BattleLootReadModel loot,
            ArenaPlayerFlowReadModel presentation,
            ArenaInputFacade input,
            ulong localSessionId)
        {
            Movement = movement ?? throw new ArgumentNullException(nameof(movement));
            MovementReadModel = movementReadModel ??
                throw new ArgumentNullException(nameof(movementReadModel));
            Combat = combat ?? throw new ArgumentNullException(nameof(combat));
            Loot = loot ?? throw new ArgumentNullException(nameof(loot));
            Presentation = presentation ??
                throw new ArgumentNullException(nameof(presentation));
            Input = input ?? throw new ArgumentNullException(nameof(input));
            if (localSessionId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(localSessionId));
            }
            LocalSessionId = localSessionId;
        }

        public BattleMovementClient Movement { get; }
        public BattleMovementReadModel MovementReadModel { get; }
        public BattleCombatReadModel Combat { get; }
        public BattleLootReadModel Loot { get; }
        public ArenaPlayerFlowReadModel Presentation { get; }
        public ArenaInputFacade Input { get; }
        public ulong LocalSessionId { get; }
        public bool IsTransportReady => Movement.IsBound;
    }

    public sealed class PlayerFlowKeyboardInput
    {
        private readonly ILobbyRoomCommands roomCommands;
        private readonly Func<ArenaInputBinding> arena;
        private readonly Action<string> showStatus;
        private Task inFlight;
        private double nextMoveAt;

        public PlayerFlowKeyboardInput(
            ILobbyRoomCommands roomCommands,
            Func<ArenaInputBinding> arena,
            Action<string> showStatus)
        {
            this.roomCommands = roomCommands ??
                throw new ArgumentNullException(nameof(roomCommands));
            this.arena = arena ?? throw new ArgumentNullException(nameof(arena));
            this.showStatus = showStatus ??
                throw new ArgumentNullException(nameof(showStatus));
        }

        public void Tick(CancellationToken cancellationToken)
        {
            ObserveInFlight();
            if (inFlight != null)
            {
                return;
            }

            ArenaInputBinding currentArena = arena();
            if (currentArena == null ||
                SceneManager.GetActiveScene().name != "ArenaScene")
            {
                return;
            }
            if (currentArena.IsTransportReady &&
                currentArena.Presentation.Snapshot().ControlsEnabled)
            {
                TickArena(currentArena, cancellationToken);
            }
        }

        private void TickArena(
            ArenaInputBinding current,
            CancellationToken cancellationToken)
        {
            if (Input.GetKeyDown(KeyCode.L))
            {
                Begin(roomCommands.LeaveAsync(cancellationToken));
                return;
            }
            if (Input.GetKeyDown(KeyCode.Space) &&
                current.Presentation.Snapshot().CanAttack)
            {
                Begin(current.Input.AttackAsync(
                    current.Combat.MonsterId, cancellationToken));
                return;
            }
            if (Input.GetKeyDown(KeyCode.E))
            {
                if (current.MovementReadModel.Positions.TryGetValue(
                        current.LocalSessionId,
                        out PlayerPosition position))
                {
                    BattleLootDropView drop = current.Loot.FindNearestAvailable(
                        position.PositionXMillimeters,
                        position.PositionYMillimeters);
                    if (drop != null)
                    {
                        Begin(current.Input.ClaimAsync(
                            drop.DropId,
                            cancellationToken));
                        return;
                    }
                }
            }
            double now = Time.realtimeSinceStartupAsDouble;
            short x = Axis(Input.GetAxisRaw("Horizontal"));
            short y = Axis(Input.GetAxisRaw("Vertical"));
            if (now >= nextMoveAt)
            {
                nextMoveAt = now + 0.1;
                Begin(current.Input.MoveAsync(x, y, cancellationToken));
                return;
            }
        }

        private void Begin(Task operation)
        {
            inFlight = operation ?? throw new ArgumentNullException(nameof(operation));
        }

        private void ObserveInFlight()
        {
            if (inFlight == null || !inFlight.IsCompleted)
            {
                return;
            }
            if (inFlight.IsFaulted || inFlight.IsCanceled)
            {
                showStatus("요청을 완료하지 못했습니다.");
            }
            inFlight = null;
        }

        private static short Axis(float value)
        {
            if (value > 0.01f)
            {
                return short.MaxValue;
            }
            if (value < -0.01f)
            {
                return -short.MaxValue;
            }
            return 0;
        }
    }
}
