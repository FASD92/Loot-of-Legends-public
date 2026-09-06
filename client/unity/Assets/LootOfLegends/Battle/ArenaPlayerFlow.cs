using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
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
            int positionYMillimeters,
            string nickname = "")
            : this(
                sessionId,
                positionXMillimeters,
                positionYMillimeters,
                nickname,
                false,
                0,
                0,
                true)
        {
        }

        public ArenaPlayerProjection(
            ulong sessionId,
            int positionXMillimeters,
            int positionYMillimeters,
            string nickname,
            bool healthKnown,
            uint hitPoints,
            uint maximumHitPoints,
            bool isAlive)
        {
            SessionId = sessionId;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
            Nickname = nickname ?? string.Empty;
            HealthKnown = healthKnown;
            HitPoints = hitPoints;
            MaximumHitPoints = maximumHitPoints;
            IsAlive = isAlive;
        }

        public ulong SessionId { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
        public string Nickname { get; }
        public bool HealthKnown { get; }
        public uint HitPoints { get; }
        public uint MaximumHitPoints { get; }
        public bool IsAlive { get; }
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
            : this(
                hasMonster,
                monsterId,
                hitPoints,
                maximumHitPoints,
                state,
                outcome,
                0,
                0)
        {
        }

        public ArenaMonsterProjection(
            bool hasMonster,
            ulong monsterId,
            uint hitPoints,
            uint maximumHitPoints,
            string state,
            string outcome,
            int positionXMillimeters,
            int positionYMillimeters)
        {
            HasMonster = hasMonster;
            MonsterId = monsterId;
            HitPoints = hitPoints;
            MaximumHitPoints = maximumHitPoints;
            State = state ?? string.Empty;
            Outcome = outcome ?? string.Empty;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
        }

        public bool HasMonster { get; }
        public ulong MonsterId { get; }
        public uint HitPoints { get; }
        public uint MaximumHitPoints { get; }
        public string State { get; }
        public string Outcome { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
    }

    public sealed class ArenaAttackProjection
    {
        public ArenaAttackProjection(
            uint sequence,
            ulong attackerSessionId,
            uint actualDamage,
            uint remainingHitPoints)
        {
            Sequence = sequence;
            AttackerSessionId = attackerSessionId;
            ActualDamage = actualDamage;
            RemainingHitPoints = remainingHitPoints;
        }

        public uint Sequence { get; }
        public ulong AttackerSessionId { get; }
        public uint ActualDamage { get; }
        public uint RemainingHitPoints { get; }
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
            string lootTerminalCopy,
            ArenaAttackProjection lastAppliedAttack = null,
            uint remainingCombatSeconds = 0,
            uint remainingLootSeconds = 0,
            ulong score = 0)
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
            LastAppliedAttack = lastAppliedAttack;
            RemainingCombatSeconds = remainingCombatSeconds;
            RemainingLootSeconds = remainingLootSeconds;
            Score = score;
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
        public ArenaAttackProjection LastAppliedAttack { get; }
        public uint RemainingCombatSeconds { get; }
        public uint RemainingLootSeconds { get; }
        public ulong Score { get; }
    }

    public sealed class ArenaPlayerFlowReadModel
    {
        private const uint CombatDurationSeconds = 30;
        private const uint LootDurationSeconds = 15;

        private readonly BattleLoadReadModel load;
        private readonly BattleMovementReadModel movement;
        private readonly BattleCombatReadModel combat;
        private readonly BattleLootReadModel loot;
        private readonly BattlePlayerStateReadModel playerState;
        private double? combatStartedAtSeconds;
        private double? lootStartedAtSeconds;
        private double? authoritativeDeadlineSeconds;
        private BattleResumePhase? authoritativePhase;
        private ulong authoritativeScore;

        public ArenaPlayerFlowReadModel(
            BattleLoadReadModel load,
            BattleMovementReadModel movement,
            BattleCombatReadModel combat,
            BattleLootReadModel loot)
            : this(load, movement, combat, loot, null)
        {
        }

        public ArenaPlayerFlowReadModel(
            BattleLoadReadModel load,
            BattleMovementReadModel movement,
            BattleCombatReadModel combat,
            BattleLootReadModel loot,
            BattlePlayerStateReadModel playerState)
        {
            this.load = load ?? throw new ArgumentNullException(nameof(load));
            this.movement = movement ?? throw new ArgumentNullException(nameof(movement));
            this.combat = combat ?? throw new ArgumentNullException(nameof(combat));
            this.loot = loot ?? throw new ArgumentNullException(nameof(loot));
            this.playerState = playerState;
        }

        public bool ApplyResumeSnapshot(BattleResumeSnapshot snapshot)
        {
            if (snapshot == null || snapshot.BattleInstanceId != load.BattleInstanceId)
            {
                return false;
            }
            authoritativePhase = snapshot.Phase;
            authoritativeScore = snapshot.Score;
            authoritativeDeadlineSeconds =
                Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency +
                snapshot.RemainingMilliseconds / 1000.0;
            combatStartedAtSeconds = null;
            lootStartedAtSeconds = null;
            return true;
        }

        public ArenaPresentationSnapshot Snapshot()
        {
            IReadOnlyDictionary<ulong, string> nicknames = load.Participants
                .ToDictionary(participant => participant.SessionId,
                    participant => participant.Nickname);
            var players = movement.Positions
                .OrderBy(entry => entry.Key)
                .Select(entry =>
                {
                    BattlePlayerStateView state = null;
                    if (playerState != null)
                    {
                        playerState.Players.TryGetValue(entry.Key, out state);
                    }
                    bool healthKnown = state != null && state.HealthKnown;
                    return new ArenaPlayerProjection(
                        entry.Key,
                        entry.Value.PositionXMillimeters,
                        entry.Value.PositionYMillimeters,
                        nicknames.TryGetValue(entry.Key, out string nickname)
                            ? nickname
                            : string.Empty,
                        healthKnown,
                        state == null ? 0 : state.HitPoints,
                        state == null ? 0 : state.MaximumHitPoints,
                        state == null || state.IsAlive);
                })
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
                combat.OutcomeName,
                combat.PositionXMillimeters,
                combat.PositionYMillimeters);
            RudpAttackApplied applied = combat.LastAppliedAttack;
            bool lootOpen = load.IsGameplayActive &&
                loot.ResolutionState == RudpLootResolutionState.Open;
            bool combatCountdownActive = load.IsGameplayActive &&
                (authoritativePhase == null || authoritativePhase == BattleResumePhase.Combat) &&
                combat.Outcome == RudpCombatOutcome.None;
            bool lootCountdownActive = lootOpen &&
                (authoritativePhase == BattleResumePhase.Loot ||
                 combat.Outcome == RudpCombatOutcome.MonsterDefeated);
            bool controlsEnabled = load.IsGameplayActive && !load.IsReconnectLocked;
            double nowSeconds = Stopwatch.GetTimestamp() /
                (double)Stopwatch.Frequency;
            combatStartedAtSeconds = combatCountdownActive && !authoritativeDeadlineSeconds.HasValue
                ? combatStartedAtSeconds ?? nowSeconds
                : null;
            lootStartedAtSeconds = lootCountdownActive && !authoritativeDeadlineSeconds.HasValue
                ? lootStartedAtSeconds ?? nowSeconds
                : null;
            uint authoritativeRemaining = authoritativeDeadlineSeconds.HasValue
                ? RemainingUntil(authoritativeDeadlineSeconds.Value, nowSeconds)
                : 0;
            return new ArenaPresentationSnapshot(
                load.IsWaiting,
                controlsEnabled,
                load.IsGameplayActive && combat.HasMonster &&
                    combat.Outcome == RudpCombatOutcome.None,
                lootOpen,
                players,
                monster,
                drops,
                combat.LastAttackResult.HasValue
                    ? "Attack: " + combat.LastAttackResult.Value
                    : "Attack: —",
                loot.LastClaimResult.HasValue
                    ? "Loot: " + loot.LastClaimResult.Value
                    : "Loot: —",
                applied == null
                    ? null
                    : new ArenaAttackProjection(
                        applied.EventSequence,
                        applied.AttackerSessionId,
                        applied.ActualDamage,
                        applied.RemainingHitPoints),
                combatCountdownActive
                    ? authoritativeDeadlineSeconds.HasValue
                        ? authoritativeRemaining
                        : RemainingSeconds(
                        CombatDurationSeconds,
                        combatStartedAtSeconds,
                        nowSeconds)
                    : 0,
                lootCountdownActive
                    ? authoritativeDeadlineSeconds.HasValue
                        ? authoritativeRemaining
                        : RemainingSeconds(
                        LootDurationSeconds,
                        lootStartedAtSeconds,
                        nowSeconds)
                    : 0,
                authoritativeScore);
        }

        private static uint RemainingUntil(double deadlineSeconds, double nowSeconds)
        {
            return deadlineSeconds <= nowSeconds
                ? 0
                : (uint)Math.Ceiling(deadlineSeconds - nowSeconds);
        }

        private static uint RemainingSeconds(
            uint durationSeconds,
            double? startedAtSeconds,
            double nowSeconds)
        {
            double elapsedSeconds = startedAtSeconds.HasValue
                ? nowSeconds - startedAtSeconds.Value
                : durationSeconds;
            return elapsedSeconds >= durationSeconds
                ? 0
                : (uint)Math.Ceiling(durationSeconds - elapsedSeconds);
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
            if (load.IsReconnectLocked)
            {
                throw new ArenaInputUnavailableException(
                    "Battle input is locked while reconnecting");
            }
            if (!load.IsGameplayActive)
            {
                throw new ArenaInputUnavailableException(
                    "Waiting for server ArenaGameplayStart");
            }
        }
    }
}
