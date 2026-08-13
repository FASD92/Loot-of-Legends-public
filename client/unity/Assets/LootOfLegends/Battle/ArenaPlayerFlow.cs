using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Transport.Rudp;

namespace LootOfLegends.Battle
{
    public sealed class ArenaPlayerProjection
    {
        public ArenaPlayerProjection(
            ulong sessionId,
            int positionXMillimeters,
            int positionYMillimeters)
        {
            SessionId = sessionId;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
        }

        public ulong SessionId { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
    }

    public sealed class ArenaMonsterProjection
    {
        public ArenaMonsterProjection(
            bool hasMonster,
            ulong monsterId,
            uint hitPoints,
            uint maximumHitPoints,
            string state,
            string outcome)
        {
            HasMonster = hasMonster;
            MonsterId = monsterId;
            HitPoints = hitPoints;
            MaximumHitPoints = maximumHitPoints;
            State = state ?? string.Empty;
            Outcome = outcome ?? string.Empty;
        }

        public bool HasMonster { get; }
        public ulong MonsterId { get; }
        public uint HitPoints { get; }
        public uint MaximumHitPoints { get; }
        public string State { get; }
        public string Outcome { get; }
    }

    public sealed class ArenaDropProjection
    {
        public ArenaDropProjection(
            ulong dropId,
            ulong itemId,
            ulong quantity,
            int positionXMillimeters,
            int positionYMillimeters,
            string state,
            ulong ownerSessionId)
        {
            DropId = dropId;
            ItemId = itemId;
            Quantity = quantity;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
            State = state;
            OwnerSessionId = ownerSessionId;
        }

        public ulong DropId { get; }
        public ulong ItemId { get; }
        public ulong Quantity { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
        public string State { get; }
        public ulong OwnerSessionId { get; }
    }

    public sealed class ArenaPresentationSnapshot
    {
        private readonly IReadOnlyList<ArenaPlayerProjection> players;
        private readonly IReadOnlyList<ArenaDropProjection> drops;

        public ArenaPresentationSnapshot(
            bool waitingForGameplayStart,
            bool controlsEnabled,
            bool canAttack,
            bool canClaimLoot,
            IReadOnlyList<ArenaPlayerProjection> players,
            ArenaMonsterProjection monster,
            IReadOnlyList<ArenaDropProjection> drops,
            string attackTerminalCopy,
            string lootTerminalCopy)
        {
            WaitingForGameplayStart = waitingForGameplayStart;
            ControlsEnabled = controlsEnabled;
            CanAttack = canAttack;
            CanClaimLoot = canClaimLoot;
            this.players = new ReadOnlyCollection<ArenaPlayerProjection>(
                new List<ArenaPlayerProjection>(players));
            Monster = monster;
            this.drops = new ReadOnlyCollection<ArenaDropProjection>(
                new List<ArenaDropProjection>(drops));
            AttackTerminalCopy = attackTerminalCopy;
            LootTerminalCopy = lootTerminalCopy;
        }

        public bool WaitingForGameplayStart { get; }
        public bool ControlsEnabled { get; }
        public bool CanAttack { get; }
        public bool CanClaimLoot { get; }
        public IReadOnlyList<ArenaPlayerProjection> Players => players;
        public ArenaMonsterProjection Monster { get; }
        public IReadOnlyList<ArenaDropProjection> Drops => drops;
        public string AttackTerminalCopy { get; }
        public string LootTerminalCopy { get; }
    }

    public sealed class ArenaPlayerFlowReadModel
    {
        private readonly BattleLoadReadModel load;
        private readonly BattleMovementReadModel movement;
        private readonly BattleCombatReadModel combat;
        private readonly BattleLootReadModel loot;

        public ArenaPlayerFlowReadModel(
            BattleLoadReadModel load,
            BattleMovementReadModel movement,
            BattleCombatReadModel combat,
            BattleLootReadModel loot)
        {
            this.load = load ?? throw new ArgumentNullException(nameof(load));
            this.movement = movement ?? throw new ArgumentNullException(nameof(movement));
            this.combat = combat ?? throw new ArgumentNullException(nameof(combat));
            this.loot = loot ?? throw new ArgumentNullException(nameof(loot));
        }

        public ArenaPresentationSnapshot Snapshot()
        {
            var players = movement.Positions
                .OrderBy(entry => entry.Key)
                .Select(entry => new ArenaPlayerProjection(
                    entry.Key,
                    entry.Value.PositionXMillimeters,
                    entry.Value.PositionYMillimeters))
                .ToList();
            var drops = loot.Drops
                .Select(drop => new ArenaDropProjection(
                    drop.DropId,
                    drop.ItemId,
                    drop.Quantity,
                    drop.PositionXMillimeters,
                    drop.PositionYMillimeters,
                    drop.StateName,
                    drop.OwnerSessionId))
                .ToList();
            var monster = new ArenaMonsterProjection(
                combat.HasMonster,
                combat.MonsterId,
                combat.HitPoints,
                combat.MaximumHitPoints,
                combat.HasMonster ? combat.MonsterState.ToString() : string.Empty,
                combat.OutcomeName);
            return new ArenaPresentationSnapshot(
                load.IsWaiting,
                load.IsGameplayActive,
                load.IsGameplayActive && combat.HasMonster &&
                    combat.Outcome == RudpCombatOutcome.None,
                load.IsGameplayActive && loot.ResolutionState == RudpLootResolutionState.Open,
                players,
                monster,
                drops,
                combat.LastAttackResult.HasValue
                    ? "Attack: " + combat.LastAttackResult.Value
                    : "Attack: —",
                loot.LastClaimResult.HasValue
                    ? "Loot: " + loot.LastClaimResult.Value
                    : "Loot: —");
        }
    }

    public sealed class ArenaInputUnavailableException : InvalidOperationException
    {
        public ArenaInputUnavailableException(string message)
            : base(message)
        {
        }
    }

    public sealed class ArenaInputFacade
    {
        private readonly BattleLoadReadModel load;
        private readonly BattleCombatReadModel combat;
        private readonly BattleLootReadModel loot;
        private readonly Func<short, short, CancellationToken, Task> move;
        private readonly Func<ulong, CancellationToken, Task> attack;
        private readonly Func<ulong, CancellationToken, Task> claim;

        public ArenaInputFacade(
            BattleLoadReadModel load,
            BattleCombatReadModel combat,
            BattleLootReadModel loot,
            Func<short, short, CancellationToken, Task> move,
            Func<ulong, CancellationToken, Task> attack,
            Func<ulong, CancellationToken, Task> claim)
        {
            this.load = load ?? throw new ArgumentNullException(nameof(load));
            this.combat = combat ?? throw new ArgumentNullException(nameof(combat));
            this.loot = loot ?? throw new ArgumentNullException(nameof(loot));
            this.move = move ?? throw new ArgumentNullException(nameof(move));
            this.attack = attack ?? throw new ArgumentNullException(nameof(attack));
            this.claim = claim ?? throw new ArgumentNullException(nameof(claim));
        }

        public Task MoveAsync(
            short desiredX,
            short desiredY,
            CancellationToken cancellationToken)
        {
            RequireGameplayStart();
            return move(desiredX, desiredY, cancellationToken);
        }

        public Task AttackAsync(ulong targetId, CancellationToken cancellationToken)
        {
            RequireGameplayStart();
            if (targetId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(targetId));
            }
            if (!combat.HasMonster || combat.Outcome != RudpCombatOutcome.None)
            {
                throw new ArenaInputUnavailableException("Attack is not available");
            }
            return attack(targetId, cancellationToken);
        }

        public Task ClaimAsync(ulong dropId, CancellationToken cancellationToken)
        {
            RequireGameplayStart();
            if (dropId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(dropId));
            }
            if (loot.ResolutionState != RudpLootResolutionState.Open)
            {
                throw new ArenaInputUnavailableException("Loot claim is not available");
            }
            return claim(dropId, cancellationToken);
        }

        private void RequireGameplayStart()
        {
            if (!load.IsGameplayActive)
            {
                throw new ArenaInputUnavailableException(
                    "Waiting for server ArenaGameplayStart");
            }
        }
    }
}
