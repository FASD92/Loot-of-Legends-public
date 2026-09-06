using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Transport;
using LootOfLegends.Transport.Rudp;

namespace LootOfLegends.Battle
{
    public sealed class ArenaClientRuntime
    {
        private readonly RudpInboundPump inbound;

        public ArenaClientRuntime(
            ITcpCommandSender tcpSender,
            IRudpDatagramSender rudpSender,
            RudpInboundPump inbound,
            BattleLoadReadModel load,
            ulong sessionId,
            ulong sessionGeneration,
            ulong battleInstanceId)
            : this(
                tcpSender,
                new RudpReliableOutbound(rudpSender, inbound),
                inbound,
                load,
                sessionId,
                sessionGeneration,
                battleInstanceId,
                1)
        {
        }

        public ArenaClientRuntime(
            ITcpCommandSender tcpSender,
            RudpReliableOutbound reliableOutbound,
            RudpInboundPump inbound,
            BattleLoadReadModel load,
            ulong sessionId,
            ulong sessionGeneration,
            ulong battleInstanceId,
            uint nextMovementActionSequence = 1)
        {
            this.inbound = inbound ?? throw new ArgumentNullException(nameof(inbound));
            SessionId = sessionId;
            SessionGeneration = sessionGeneration;
            Movement = new BattleMovementClient(
                tcpSender,
                reliableOutbound,
                inbound,
                sessionId,
                sessionGeneration,
                battleInstanceId,
                nextMovementActionSequence);
            Combat = new BattleCombatReadModel(battleInstanceId);
            Loot = new BattleLootReadModel(battleInstanceId);
            PlayerState = new BattlePlayerStateReadModel();

            var attack = new AttackInputFacade(
                reliableOutbound,
                battleInstanceId);
            var claim = new ClaimLootInputFacade(
                reliableOutbound,
                battleInstanceId);
            Input = new ArenaInputFacade(
                load,
                Combat,
                Loot,
                Movement.SendDirectionAsync,
                async (targetId, cancellationToken) =>
                {
                    await attack.AttackAsync(targetId, cancellationToken)
                        .ConfigureAwait(false);
                },
                async (dropId, cancellationToken) =>
                {
                    await claim.ClaimAsync(dropId, cancellationToken)
                        .ConfigureAwait(false);
                });
            Presentation = new ArenaPlayerFlowReadModel(
                load,
                Movement.ReadModel,
                Combat,
                Loot,
                PlayerState);
        }

        public BattleMovementClient Movement { get; }
        public BattleCombatReadModel Combat { get; }
        public BattleLootReadModel Loot { get; }
        public BattlePlayerStateReadModel PlayerState { get; }
        public ArenaInputFacade Input { get; }
        public ArenaPlayerFlowReadModel Presentation { get; }
        public bool IsTransportReady => Movement.IsBound;
        public ulong SessionId { get; }
        public ulong SessionGeneration { get; }

        public bool ApplyResumeSnapshot(BattleResumeSnapshot snapshot)
        {
            if (snapshot == null)
            {
                throw new ArgumentNullException(nameof(snapshot));
            }
            return Movement.ReadModel.ApplyResumeSnapshot(snapshot) &&
                PlayerState.ApplyResumeSnapshot(snapshot) &&
                Combat.ApplyResumeSnapshot(snapshot) &&
                Loot.ApplyResumeSnapshot(snapshot) &&
                Presentation.ApplyResumeSnapshot(snapshot);
        }

        public Task RequestTransportAsync(
            ulong requestId,
            CancellationToken cancellationToken)
        {
            return Movement.RequestBindCapabilityAsync(requestId, cancellationToken);
        }

        public async Task DrainAsync(CancellationToken cancellationToken)
        {
            await Movement.DrainBindCapabilitiesAsync(cancellationToken);
            Movement.DrainInbound();
            while (inbound.TryDequeueCombat(out RudpInboundDatagram combat))
            {
                Combat.Apply(combat.Message);
            }
            while (inbound.TryDequeueLoot(out RudpInboundDatagram loot))
            {
                Loot.Apply(loot.Message);
            }
        }

        public Task TickAsync(
            long nowMilliseconds,
            CancellationToken cancellationToken)
        {
            return Movement.TickAsync(nowMilliseconds, cancellationToken);
        }
    }
}
