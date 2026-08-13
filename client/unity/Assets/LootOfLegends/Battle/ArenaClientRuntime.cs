using System;
using System.Threading;
using System.Threading.Tasks;
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
                battleInstanceId)
        {
        }

        public ArenaClientRuntime(
            ITcpCommandSender tcpSender,
            RudpReliableOutbound reliableOutbound,
            RudpInboundPump inbound,
            BattleLoadReadModel load,
            ulong sessionId,
            ulong sessionGeneration,
            ulong battleInstanceId)
        {
            this.inbound = inbound ?? throw new ArgumentNullException(nameof(inbound));
            Movement = new BattleMovementClient(
                tcpSender,
                reliableOutbound,
                inbound,
                sessionId,
                sessionGeneration,
                battleInstanceId);
            Combat = new BattleCombatReadModel(battleInstanceId);
            Loot = new BattleLootReadModel(battleInstanceId);

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
                Loot);
        }

        public BattleMovementClient Movement { get; }
        public BattleCombatReadModel Combat { get; }
        public BattleLootReadModel Loot { get; }
        public ArenaInputFacade Input { get; }
        public ArenaPlayerFlowReadModel Presentation { get; }
        public bool IsTransportReady => Movement.IsBound;

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
