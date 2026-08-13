using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Transport;
using LootOfLegends.Transport.Rudp;

namespace LootOfLegends.Battle.Movement
{
    public sealed class PlayerPosition
    {
        internal PlayerPosition(int positionXMillimeters, int positionYMillimeters)
        {
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
        }

        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
    }

    public sealed class BattleMovementReadModel
    {
        private readonly ulong battleInstanceId;
        private readonly int ownerThreadId;
        private readonly Dictionary<ulong, PlayerPosition> positions =
            new Dictionary<ulong, PlayerPosition>();
        private readonly IReadOnlyDictionary<ulong, PlayerPosition> readOnlyPositions;
        private bool hasSnapshot;

        public BattleMovementReadModel(ulong battleInstanceId)
        {
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            this.battleInstanceId = battleInstanceId;
            ownerThreadId = Thread.CurrentThread.ManagedThreadId;
            readOnlyPositions = new ReadOnlyDictionary<ulong, PlayerPosition>(positions);
        }

        public uint SnapshotSequence { get; private set; }
        public uint ServerTick { get; private set; }
        public IReadOnlyDictionary<ulong, PlayerPosition> Positions => readOnlyPositions;

        public bool Apply(RudpStateSnapshot snapshot)
        {
            if (Thread.CurrentThread.ManagedThreadId != ownerThreadId)
            {
                throw new InvalidOperationException("Movement read model must update on its owner thread");
            }
            if (snapshot == null)
            {
                throw new ArgumentNullException(nameof(snapshot));
            }
            if (snapshot.BattleInstanceId != battleInstanceId ||
                (hasSnapshot &&
                 !RudpProtocolCodec.IsSequenceNewer(snapshot.SnapshotSequence, SnapshotSequence)))
            {
                return false;
            }

            positions.Clear();
            foreach (RudpSnapshotPlayer player in snapshot.Players)
            {
                positions.Add(
                    player.SessionId,
                    new PlayerPosition(
                        player.PositionXMillimeters,
                        player.PositionYMillimeters));
            }
            SnapshotSequence = snapshot.SnapshotSequence;
            ServerTick = snapshot.ServerTick;
            hasSnapshot = true;
            return true;
        }
    }

    public sealed class BattleMovementClient : IRudpBindCapabilitySink
    {
        private const long HeartbeatIntervalMilliseconds = 1000;

        private readonly ITcpCommandSender tcpSender;
        private readonly RudpReliableOutbound reliableOutbound;
        private readonly RudpInboundPump inboundPump;
        private readonly ulong sessionId;
        private readonly ulong sessionGeneration;
        private readonly ulong battleInstanceId;
        private RudpBindCapability pendingBindCapability;
        private ulong pendingCapabilityRequestId;
        private uint nextActionSequence = 1;
        private long? lastHeartbeatMilliseconds;

        public BattleMovementClient(
            ITcpCommandSender tcpSender,
            IRudpDatagramSender rudpSender,
            RudpInboundPump inboundPump,
            ulong sessionId,
            ulong sessionGeneration,
            ulong battleInstanceId)
            : this(
                tcpSender,
                new RudpReliableOutbound(rudpSender, inboundPump),
                inboundPump,
                sessionId,
                sessionGeneration,
                battleInstanceId)
        {
        }

        public BattleMovementClient(
            ITcpCommandSender tcpSender,
            RudpReliableOutbound reliableOutbound,
            RudpInboundPump inboundPump,
            ulong sessionId,
            ulong sessionGeneration,
            ulong battleInstanceId)
        {
            this.tcpSender = tcpSender ?? throw new ArgumentNullException(nameof(tcpSender));
            this.reliableOutbound = reliableOutbound ??
                throw new ArgumentNullException(nameof(reliableOutbound));
            this.inboundPump = inboundPump ?? throw new ArgumentNullException(nameof(inboundPump));
            if (sessionId == 0 || sessionGeneration == 0 || battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(sessionId));
            }
            this.sessionId = sessionId;
            this.sessionGeneration = sessionGeneration;
            this.battleInstanceId = battleInstanceId;
            ReadModel = new BattleMovementReadModel(battleInstanceId);
        }

        public bool IsBound => reliableOutbound.TransportEpoch != 0;
        public uint TransportEpoch => reliableOutbound.TransportEpoch;
        public BattleMovementReadModel ReadModel { get; }

        public async Task RequestBindCapabilityAsync(
            ulong requestId,
            CancellationToken cancellationToken)
        {
            if (requestId == 0 || pendingCapabilityRequestId != 0)
            {
                throw new InvalidOperationException("RUDP bind request is not valid in the current state");
            }
            pendingCapabilityRequestId = requestId;
            try
            {
                reliableOutbound.PrepareForRebind();
                inboundPump.PrepareForRebind();
                await tcpSender.SendAsync(
                    RudpProtocolCodec.EncodeBindCapabilityRequest(requestId),
                    cancellationToken).ConfigureAwait(false);
            }
            catch
            {
                pendingCapabilityRequestId = 0;
                throw;
            }
        }

        public async Task AcceptBindCapabilityAsync(
            byte[] frame,
            CancellationToken cancellationToken)
        {
            RudpBindCapability capability = RudpProtocolCodec.DecodeBindCapability(frame);
            await AcceptBindCapabilityAsync(capability, cancellationToken).ConfigureAwait(false);
        }

        public void OnRudpBindCapability(RudpBindCapability capability)
        {
            if (capability == null)
            {
                throw new ArgumentNullException(nameof(capability));
            }
            Interlocked.Exchange(ref pendingBindCapability, capability);
        }

        public async Task<int> DrainBindCapabilitiesAsync(CancellationToken cancellationToken)
        {
            RudpBindCapability capability =
                Interlocked.Exchange(ref pendingBindCapability, null);
            if (capability == null)
            {
                return 0;
            }
            await AcceptBindCapabilityAsync(capability, cancellationToken)
                .ConfigureAwait(false);
            return 1;
        }

        private async Task AcceptBindCapabilityAsync(
            RudpBindCapability capability,
            CancellationToken cancellationToken)
        {
            if (capability.RequestId != pendingCapabilityRequestId || IsBound)
            {
                throw new InvalidOperationException("RUDP bind capability correlation is stale");
            }
            pendingCapabilityRequestId = 0;
            await reliableOutbound.SendBindHelloAsync(
                    capability.Bytes,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        public int DrainInbound()
        {
            int applied = 0;
            while (inboundPump.TryDequeueMovement(out RudpInboundDatagram datagram))
            {
                if (ApplyInbound(datagram))
                {
                    applied++;
                }
            }
            return applied;
        }

        public bool ApplyInbound(RudpInboundDatagram datagram)
        {
            if (datagram == null)
            {
                throw new ArgumentNullException(nameof(datagram));
            }
            RudpHeader header = datagram.Header;
            if (header.SessionId != sessionId || header.SessionGeneration != sessionGeneration)
            {
                return false;
            }

            if (datagram.Message is RudpBindAccepted)
            {
                if (reliableOutbound.BindHelloSequence == 0 ||
                    !RudpProtocolCodec.IsAcknowledged(
                        reliableOutbound.BindHelloSequence,
                        header.Ack,
                        header.AckBits) ||
                    (TransportEpoch != 0 && TransportEpoch != header.TransportEpoch))
                {
                    return false;
                }
                inboundPump.ObserveAccepted(datagram);
                reliableOutbound.Acknowledge(header.Ack, header.AckBits);
                if (TransportEpoch != 0)
                {
                    return false;
                }
                reliableOutbound.AcceptTransportEpoch(header.TransportEpoch);
                lastHeartbeatMilliseconds = null;
                return true;
            }

            if (!(datagram.Message is RudpStateSnapshot snapshot) ||
                !IsBound || header.TransportEpoch != TransportEpoch)
            {
                return false;
            }
            inboundPump.ObserveAccepted(datagram);
            return ReadModel.Apply(snapshot);
        }

        public async Task TickAsync(long nowMilliseconds, CancellationToken cancellationToken)
        {
            await reliableOutbound.TickAsync(cancellationToken).ConfigureAwait(false);
            if (reliableOutbound.HasConfirmedFailure)
            {
                return;
            }
            if (!IsBound ||
                (lastHeartbeatMilliseconds.HasValue &&
                 nowMilliseconds - lastHeartbeatMilliseconds.Value < HeartbeatIntervalMilliseconds))
            {
                return;
            }
            await reliableOutbound.SendHeartbeatAsync(cancellationToken)
                .ConfigureAwait(false);
            lastHeartbeatMilliseconds = nowMilliseconds;
        }

        public async Task SendDirectionAsync(
            short desiredX,
            short desiredY,
            CancellationToken cancellationToken)
        {
            if (!IsBound)
            {
                throw new InvalidOperationException("RUDP peer is not bound");
            }
            uint actionSequence = TakeActionSequence();
            await reliableOutbound.SendMoveAsync(
                    battleInstanceId,
                    actionSequence,
                    desiredX,
                    desiredY,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        private uint TakeActionSequence()
        {
            uint value = nextActionSequence;
            nextActionSequence = value == uint.MaxValue ? 1 : value + 1;
            return value;
        }
    }
}
