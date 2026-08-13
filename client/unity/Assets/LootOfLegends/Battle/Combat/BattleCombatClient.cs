using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Transport.Rudp;

namespace LootOfLegends.Battle.Combat
{
    public sealed class AttackInputFacade
    {
        private readonly IRudpDatagramSender sender;
        private readonly Func<RudpHeader> nextHeader;
        private readonly RudpReliableOutbound reliableOutbound;
        private readonly ulong battleInstanceId;

        public AttackInputFacade(
            IRudpDatagramSender sender,
            Func<RudpHeader> nextHeader,
            ulong battleInstanceId)
        {
            this.sender = sender ?? throw new ArgumentNullException(nameof(sender));
            this.nextHeader = nextHeader ?? throw new ArgumentNullException(nameof(nextHeader));
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            this.battleInstanceId = battleInstanceId;
        }

        public AttackInputFacade(
            RudpReliableOutbound reliableOutbound,
            ulong battleInstanceId)
        {
            this.reliableOutbound = reliableOutbound ??
                throw new ArgumentNullException(nameof(reliableOutbound));
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            this.battleInstanceId = battleInstanceId;
        }

        public async Task<RudpCommandId> AttackAsync(
            ulong targetHint,
            CancellationToken cancellationToken)
        {
            if (targetHint == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(targetHint));
            }
            if (reliableOutbound != null)
            {
                return await reliableOutbound.SendAttackAsync(
                        battleInstanceId,
                        targetHint,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            RudpCommandId commandId = RudpCommandId.Create();
            byte[] datagram = RudpProtocolCodec.EncodeAttackIntent(
                nextHeader(), commandId, battleInstanceId, targetHint);
            await sender.SendAsync(datagram, cancellationToken).ConfigureAwait(false);
            return commandId;
        }
    }

    public sealed class BattleCombatReadModel
    {
        private const uint FrozenMaximumHitPoints = 1600;

        private readonly ulong battleInstanceId;
        private readonly int ownerThreadId;
        private bool hasSnapshot;
        private uint lifecycleSequence;
        private RudpCombatTerminalEvent pendingLifecycleTerminal;

        public BattleCombatReadModel(ulong battleInstanceId)
        {
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            this.battleInstanceId = battleInstanceId;
            ownerThreadId = Thread.CurrentThread.ManagedThreadId;
        }

        public bool HasMonster { get; private set; }
        public ulong MonsterId { get; private set; }
        public uint HitPoints { get; private set; }
        public uint MaximumHitPoints { get; private set; }
        public RudpMonsterState MonsterState { get; private set; }
        public RudpCombatOutcome Outcome { get; private set; }
        public string OutcomeName => Outcome == RudpCombatOutcome.None
            ? string.Empty
            : Outcome.ToString();
        public RudpCommandId LastAttackCommandId { get; private set; }
        public RudpAttackResultCode? LastAttackResult { get; private set; }
        public uint SnapshotSequence { get; private set; }
        public uint ServerTick { get; private set; }

        public bool Apply(object serverMessage)
        {
            if (Thread.CurrentThread.ManagedThreadId != ownerThreadId)
            {
                throw new InvalidOperationException("Combat read model must update on its owner thread");
            }
            if (serverMessage == null)
            {
                throw new ArgumentNullException(nameof(serverMessage));
            }

            switch (serverMessage)
            {
                case RudpMonsterSpawned spawned:
                    return Apply(spawned);
                case RudpAttackTerminalResult result:
                    return Apply(result);
                case RudpCombatTerminalEvent terminal:
                    return Apply(terminal);
                case RudpMonsterStateSnapshot snapshot:
                    return Apply(snapshot);
                default:
                    return false;
            }
        }

        private bool Apply(RudpMonsterSpawned spawned)
        {
            if (spawned.BattleInstanceId != battleInstanceId ||
                spawned.EventSequence <= lifecycleSequence ||
                (HasMonster && MonsterId != spawned.MonsterId) ||
                (pendingLifecycleTerminal != null &&
                 (pendingLifecycleTerminal.MonsterId != spawned.MonsterId ||
                  (Outcome != RudpCombatOutcome.None &&
                   Outcome != pendingLifecycleTerminal.CombatOutcome))))
            {
                return false;
            }
            lifecycleSequence = spawned.EventSequence;
            if (!HasMonster)
            {
                HasMonster = true;
                MonsterId = spawned.MonsterId;
                HitPoints = spawned.MaximumHitPoints;
                MonsterState = RudpMonsterState.Alive;
            }
            MaximumHitPoints = spawned.MaximumHitPoints;
            if (pendingLifecycleTerminal != null)
            {
                RudpCombatTerminalEvent terminal = pendingLifecycleTerminal;
                pendingLifecycleTerminal = null;
                CommitTerminal(terminal);
            }
            return true;
        }

        private bool Apply(RudpAttackTerminalResult result)
        {
            if (result.BattleInstanceId != battleInstanceId ||
                (HasMonster && MonsterId != result.MonsterId))
            {
                return false;
            }
            LastAttackCommandId = result.CommandId;
            LastAttackResult = result.ResultCode;
            if (Outcome != RudpCombatOutcome.None)
            {
                return result.CombatOutcome == RudpCombatOutcome.None ||
                       result.CombatOutcome == Outcome;
            }
            uint nextHitPoints = HasMonster
                ? Math.Min(HitPoints, result.RemainingHitPoints)
                : result.RemainingHitPoints;
            HasMonster = true;
            MonsterId = result.MonsterId;
            MaximumHitPoints = FrozenMaximumHitPoints;
            HitPoints = nextHitPoints;
            if (result.CombatOutcome == RudpCombatOutcome.MonsterDefeated)
            {
                Outcome = result.CombatOutcome;
                MonsterState = RudpMonsterState.Dead;
            }
            else if (result.CombatOutcome == RudpCombatOutcome.CombatTimeout)
            {
                Outcome = result.CombatOutcome;
                MonsterState = RudpMonsterState.TimedOut;
            }
            return true;
        }

        private bool Apply(RudpCombatTerminalEvent terminal)
        {
            if (terminal.BattleInstanceId != battleInstanceId ||
                terminal.EventSequence <= lifecycleSequence ||
                (HasMonster && MonsterId != terminal.MonsterId) ||
                (Outcome != RudpCombatOutcome.None && Outcome != terminal.CombatOutcome))
            {
                return false;
            }
            if (lifecycleSequence == 0)
            {
                if (pendingLifecycleTerminal != null)
                {
                    return false;
                }
                pendingLifecycleTerminal = terminal;
                return true;
            }
            CommitTerminal(terminal);
            return true;
        }

        private void CommitTerminal(RudpCombatTerminalEvent terminal)
        {
            lifecycleSequence = terminal.EventSequence;
            Outcome = terminal.CombatOutcome;
            ServerTick = terminal.ServerTick;
            if (HasMonster && terminal.CombatOutcome == RudpCombatOutcome.MonsterDefeated)
            {
                HitPoints = 0;
                MonsterState = RudpMonsterState.Dead;
            }
            else if (HasMonster && terminal.CombatOutcome == RudpCombatOutcome.CombatTimeout)
            {
                MonsterState = RudpMonsterState.TimedOut;
            }
        }

        private bool Apply(RudpMonsterStateSnapshot snapshot)
        {
            if (snapshot.BattleInstanceId != battleInstanceId ||
                Outcome != RudpCombatOutcome.None ||
                (HasMonster && MonsterId != snapshot.MonsterId) ||
                (hasSnapshot &&
                 !RudpProtocolCodec.IsSequenceNewer(
                     snapshot.SnapshotSequence, SnapshotSequence)))
            {
                return false;
            }
            HasMonster = true;
            MonsterId = snapshot.MonsterId;
            MaximumHitPoints = FrozenMaximumHitPoints;
            HitPoints = snapshot.HitPoints;
            MonsterState = snapshot.MonsterState;
            SnapshotSequence = snapshot.SnapshotSequence;
            ServerTick = snapshot.ServerTick;
            hasSnapshot = true;
            return true;
        }
    }
}
