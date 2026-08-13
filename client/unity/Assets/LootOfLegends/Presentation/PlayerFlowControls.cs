using System;
using System.Linq;
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
            ArenaInputFacade input)
        {
            Movement = movement ?? throw new ArgumentNullException(nameof(movement));
            MovementReadModel = movementReadModel ??
                throw new ArgumentNullException(nameof(movementReadModel));
            Combat = combat ?? throw new ArgumentNullException(nameof(combat));
            Loot = loot ?? throw new ArgumentNullException(nameof(loot));
            Presentation = presentation ??
                throw new ArgumentNullException(nameof(presentation));
            Input = input ?? throw new ArgumentNullException(nameof(input));
        }

        public BattleMovementClient Movement { get; }
        public BattleMovementReadModel MovementReadModel { get; }
        public BattleCombatReadModel Combat { get; }
        public BattleLootReadModel Loot { get; }
        public ArenaPlayerFlowReadModel Presentation { get; }
        public ArenaInputFacade Input { get; }
        public bool IsTransportReady => Movement.IsBound;
    }

    public sealed class PlayerFlowKeyboardInput
    {
        private readonly LobbyRoomReadModel lobbyRoom;
        private readonly ILobbyRoomCommands roomCommands;
        private readonly IRoomHostStartAction hostStart;
        private readonly Func<ArenaInputBinding> arena;
        private readonly Action<string> showStatus;
        private Task inFlight;
        private double nextMoveAt;

        public PlayerFlowKeyboardInput(
            LobbyRoomReadModel lobbyRoom,
            ILobbyRoomCommands roomCommands,
            IRoomHostStartAction hostStart,
            Func<ArenaInputBinding> arena,
            Action<string> showStatus)
        {
            this.lobbyRoom = lobbyRoom ??
                throw new ArgumentNullException(nameof(lobbyRoom));
            this.roomCommands = roomCommands ??
                throw new ArgumentNullException(nameof(roomCommands));
            this.hostStart = hostStart ??
                throw new ArgumentNullException(nameof(hostStart));
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
            if (currentArena != null &&
                SceneManager.GetActiveScene().name == "ArenaScene")
            {
                if (currentArena.IsTransportReady &&
                    currentArena.Presentation.Snapshot().ControlsEnabled)
                {
                    TickArena(currentArena, cancellationToken);
                }
                return;
            }
            if (lobbyRoom.IsInRoom)
            {
                TickRoom(cancellationToken);
                return;
            }
            if (Input.GetKeyDown(KeyCode.C))
            {
                Begin(roomCommands.CreateAsync(
                    "Player Room", 2, cancellationToken));
            }
            else if (Input.GetKeyDown(KeyCode.J))
            {
                LobbyRoomSummaryView room = lobbyRoom.Lobby.Rooms.FirstOrDefault(
                    candidate => !candidate.IsFull);
                if (room != null)
                {
                    Begin(roomCommands.JoinAsync(room.RoomId, cancellationToken));
                }
            }
        }

        private void TickRoom(CancellationToken cancellationToken)
        {
            if (Input.GetKeyDown(KeyCode.R))
            {
                RoomMemberPresentation local = lobbyRoom.Room?.Members.FirstOrDefault(
                    member => member.IsLocal);
                if (local != null)
                {
                    Begin(roomCommands.SetReadyAsync(!local.Ready, cancellationToken));
                }
            }
            else if (Input.GetKeyDown(KeyCode.S))
            {
                Begin(hostStart.StartAsync(cancellationToken));
            }
            else if (Input.GetKeyDown(KeyCode.L))
            {
                Begin(roomCommands.LeaveAsync(cancellationToken));
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
            if (Input.GetKeyDown(KeyCode.Space) && current.Combat.HasMonster)
            {
                Begin(current.Input.AttackAsync(
                    current.Combat.MonsterId, cancellationToken));
                return;
            }
            if (Input.GetKeyDown(KeyCode.E))
            {
                BattleLootDropView drop = current.Loot.Drops.FirstOrDefault(
                    candidate => candidate.IsAvailable);
                if (drop != null)
                {
                    Begin(current.Input.ClaimAsync(drop.DropId, cancellationToken));
                    return;
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
