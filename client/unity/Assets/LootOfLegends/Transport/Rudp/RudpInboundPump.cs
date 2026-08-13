using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.Transport.Rudp
{
    public interface IRudpDatagramSender
    {
        Task SendAsync(byte[] datagram, CancellationToken cancellationToken);
    }

    public sealed class UdpDatagramSender : IRudpDatagramSender
    {
        private readonly UdpClient client;

        public UdpDatagramSender(UdpClient client)
        {
            this.client = client ?? throw new ArgumentNullException(nameof(client));
        }

        public async Task SendAsync(byte[] datagram, CancellationToken cancellationToken)
        {
            if (datagram == null || datagram.Length < 48 || datagram.Length > 1200)
            {
                throw new ArgumentException("RUDP datagram is outside bounds", nameof(datagram));
            }
            cancellationToken.ThrowIfCancellationRequested();
            await client.SendAsync(datagram, datagram.Length).ConfigureAwait(false);
        }
    }

    public sealed class RudpReliableOutbound
    {
        private const int TotalCapacity = 256;
        private const int ApplicationCapacity = 224;
        private const int ByteCapacity = 256 * 1024;
        private const long InitialRtoMilliseconds = 200;
        private const long MaximumRtoMilliseconds = 1000;
        private const long ExpiryMilliseconds = 5000;
        private const int MaximumTransmissions = 5;

        private readonly object gate = new object();
        private readonly IRudpDatagramSender sender;
        private readonly RudpInboundPump inbound;
        private readonly Func<long> clock;
        private readonly List<PendingReliable> pending =
            new List<PendingReliable>();
        private uint nextSequence = 1;
        private uint transportEpoch;
        private uint bindHelloSequence;
        private int applicationCount;
        private int pendingBytes;
        private bool confirmedFailure;

        public RudpReliableOutbound(
            IRudpDatagramSender sender,
            RudpInboundPump inbound,
            Func<long> clock = null)
        {
            this.sender = sender ?? throw new ArgumentNullException(nameof(sender));
            this.inbound = inbound ?? throw new ArgumentNullException(nameof(inbound));
            this.clock = clock ?? MonotonicMilliseconds;
            inbound.AttachReliableOutbound(this);
        }

        public int PendingReliableCount
        {
            get
            {
                lock (gate)
                {
                    return pending.Count;
                }
            }
        }

        public bool HasConfirmedFailure
        {
            get
            {
                lock (gate)
                {
                    return confirmedFailure;
                }
            }
        }

        public uint TransportEpoch
        {
            get
            {
                lock (gate)
                {
                    return transportEpoch;
                }
            }
        }

        public uint BindHelloSequence
        {
            get
            {
                lock (gate)
                {
                    return bindHelloSequence;
                }
            }
        }

        public Task SendBindHelloAsync(
            byte[] capability,
            CancellationToken cancellationToken)
        {
            return EnqueueAsync(
                0,
                false,
                null,
                header => RudpProtocolCodec.EncodeBindHello(header, capability),
                cancellationToken);
        }

        public async Task<RudpCommandId> SendAttackAsync(
            ulong battleInstanceId,
            ulong targetHint,
            CancellationToken cancellationToken)
        {
            if (battleInstanceId == 0 || targetHint == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            RudpCommandId commandId = RudpCommandId.Create();
            await EnqueueAsync(
                    27,
                    true,
                    commandId,
                    header => RudpProtocolCodec.EncodeAttackIntent(
                        header, commandId, battleInstanceId, targetHint),
                    cancellationToken)
                .ConfigureAwait(false);
            return commandId;
        }

        public async Task<RudpCommandId> SendClaimLootAsync(
            ulong battleInstanceId,
            ulong dropId,
            CancellationToken cancellationToken)
        {
            if (battleInstanceId == 0 || dropId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            RudpCommandId commandId = RudpCommandId.Create();
            await EnqueueAsync(
                    32,
                    true,
                    commandId,
                    header => RudpProtocolCodec.EncodeClaimLootIntent(
                        header, commandId, battleInstanceId, dropId),
                    cancellationToken)
                .ConfigureAwait(false);
            return commandId;
        }

        public Task SendHeartbeatAsync(CancellationToken cancellationToken)
        {
            return SendUnreliableAsync(
                RudpFlag.Heartbeat,
                24,
                RudpProtocolCodec.EncodeHeartbeat,
                cancellationToken);
        }

        public Task SendMoveAsync(
            ulong battleInstanceId,
            uint actionSequence,
            short desiredX,
            short desiredY,
            CancellationToken cancellationToken)
        {
            return SendUnreliableAsync(
                RudpFlag.Unreliable,
                25,
                header => RudpProtocolCodec.EncodeMoveIntent(
                    header,
                    battleInstanceId,
                    actionSequence,
                    desiredX,
                    desiredY),
                cancellationToken);
        }

        public void AcceptTransportEpoch(uint acceptedEpoch)
        {
            if (acceptedEpoch == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(acceptedEpoch));
            }
            lock (gate)
            {
                if (confirmedFailure ||
                    (transportEpoch != 0 && transportEpoch != acceptedEpoch))
                {
                    throw new InvalidOperationException(
                        "RUDP transport epoch cannot change in the current session");
                }
                transportEpoch = acceptedEpoch;
            }
        }

        public void Acknowledge(uint ack, uint ackBits)
        {
            lock (gate)
            {
                for (int index = pending.Count - 1; index >= 0; index--)
                {
                    PendingReliable entry = pending[index];
                    if (!RudpProtocolCodec.IsAcknowledged(
                            entry.Sequence, ack, ackBits))
                    {
                        continue;
                    }
                    pending.RemoveAt(index);
                    pendingBytes -= entry.Datagram.Length;
                    if (entry.Application)
                    {
                        applicationCount--;
                    }
                }
            }
        }

        public void PrepareForRebind()
        {
            lock (gate)
            {
                if (confirmedFailure)
                {
                    throw new InvalidOperationException(
                        "RUDP reliable transport has failed");
                }
                pending.Clear();
                applicationCount = 0;
                pendingBytes = 0;
                transportEpoch = 0;
                bindHelloSequence = 0;
            }
        }

        public async Task TickAsync(CancellationToken cancellationToken)
        {
            var due = new List<byte[]>();
            long now = clock();
            lock (gate)
            {
                if (confirmedFailure)
                {
                    return;
                }
                for (int index = 0; index < pending.Count; index++)
                {
                    PendingReliable entry = pending[index];
                    if (now - entry.CreatedAtMilliseconds >= ExpiryMilliseconds)
                    {
                        FailLocked();
                        return;
                    }
                }
                foreach (PendingReliable entry in pending)
                {
                    if (entry.Transmissions >= MaximumTransmissions ||
                        now < entry.NextSendAtMilliseconds)
                    {
                        continue;
                    }
                    entry.Transmissions++;
                    if (entry.Transmissions < MaximumTransmissions)
                    {
                        entry.RtoMilliseconds = Math.Min(
                            MaximumRtoMilliseconds,
                            entry.RtoMilliseconds * 2);
                        entry.NextSendAtMilliseconds =
                            now + entry.RtoMilliseconds;
                    }
                    due.Add(entry.Datagram);
                }
            }
            foreach (byte[] datagram in due)
            {
                await sender.SendAsync(datagram, cancellationToken)
                    .ConfigureAwait(false);
            }
        }

        private async Task EnqueueAsync(
            ushort messageId,
            bool application,
            RudpCommandId commandId,
            Func<RudpHeader, byte[]> encode,
            CancellationToken cancellationToken)
        {
            byte[] datagram;
            lock (gate)
            {
                if (confirmedFailure)
                {
                    throw new InvalidOperationException(
                        "RUDP reliable transport has failed");
                }
                if ((messageId == 0 && transportEpoch != 0) ||
                    (messageId != 0 && transportEpoch == 0))
                {
                    throw new InvalidOperationException(
                        "RUDP reliable command is not valid in the current bind state");
                }
                uint sequence = TakeSequenceLocked();
                if (messageId == 0)
                {
                    bindHelloSequence = sequence;
                }
                RudpHeader header = inbound.CreateOutboundHeader(
                    RudpFlag.Reliable,
                    messageId == 0 ? 0 : transportEpoch,
                    sequence,
                    messageId == 0 ? (ushort)22 : messageId);
                datagram = encode(header);
                if (pending.Count >= TotalCapacity ||
                    (application && applicationCount >= ApplicationCapacity) ||
                    pendingBytes + datagram.Length > ByteCapacity)
                {
                    FailLocked();
                    throw new InvalidOperationException(
                        "RUDP reliable outbound queue is overloaded");
                }
                long now = clock();
                pending.Add(new PendingReliable(
                    sequence,
                    datagram,
                    commandId,
                    application,
                    now));
                pendingBytes += datagram.Length;
                if (application)
                {
                    applicationCount++;
                }
            }
            await sender.SendAsync(datagram, cancellationToken)
                .ConfigureAwait(false);
        }

        private async Task SendUnreliableAsync(
            RudpFlag flag,
            ushort messageId,
            Func<RudpHeader, byte[]> encode,
            CancellationToken cancellationToken)
        {
            byte[] datagram;
            lock (gate)
            {
                if (confirmedFailure || transportEpoch == 0)
                {
                    throw new InvalidOperationException("RUDP peer is not bound");
                }
                datagram = encode(inbound.CreateOutboundHeader(
                    flag,
                    transportEpoch,
                    TakeSequenceLocked(),
                    messageId));
            }
            await sender.SendAsync(datagram, cancellationToken)
                .ConfigureAwait(false);
        }

        private uint TakeSequenceLocked()
        {
            uint value = nextSequence;
            nextSequence = value == uint.MaxValue ? 1 : value + 1;
            return value;
        }

        private void FailLocked()
        {
            confirmedFailure = true;
            pending.Clear();
            applicationCount = 0;
            pendingBytes = 0;
        }

        private static long MonotonicMilliseconds()
        {
            return (long)(Stopwatch.GetTimestamp() *
                          (1000.0 / Stopwatch.Frequency));
        }

        private sealed class PendingReliable
        {
            public PendingReliable(
                uint sequence,
                byte[] datagram,
                RudpCommandId commandId,
                bool application,
                long createdAtMilliseconds)
            {
                Sequence = sequence;
                Datagram = datagram;
                CommandId = commandId;
                Application = application;
                CreatedAtMilliseconds = createdAtMilliseconds;
                NextSendAtMilliseconds =
                    createdAtMilliseconds + InitialRtoMilliseconds;
                RtoMilliseconds = InitialRtoMilliseconds;
                Transmissions = 1;
            }

            public uint Sequence { get; }
            public byte[] Datagram { get; }
            public RudpCommandId CommandId { get; }
            public bool Application { get; }
            public long CreatedAtMilliseconds { get; }
            public long NextSendAtMilliseconds { get; set; }
            public long RtoMilliseconds { get; set; }
            public int Transmissions { get; set; }
        }
    }

    public sealed class RudpInboundPump
    {
        private static readonly ConditionalWeakTable<UdpClient, RunOwnership> Ownership =
            new ConditionalWeakTable<UdpClient, RunOwnership>();
        private const int CombatReliableCapacity = 256;
        private const int LootReliableCapacity = 256;

        private readonly UdpClient client;
        private readonly IPEndPoint expectedServer;
        private readonly ulong sessionId;
        private readonly ulong sessionGeneration;
        private readonly object inboundGate = new object();
        private readonly RudpReceiveHistory receiveHistory = new RudpReceiveHistory();
        private readonly Queue<RudpInboundDatagram> pendingCombat =
            new Queue<RudpInboundDatagram>();
        private readonly Queue<RudpInboundDatagram> pendingLoot =
            new Queue<RudpInboundDatagram>();
        private RudpInboundDatagram pendingControl;
        private RudpInboundDatagram latestMovementSnapshot;
        private RudpInboundDatagram latestCombatSnapshot;
        private RudpInboundDatagram latestLootSnapshot;
        private uint transportEpoch;
        private bool rebindPrepared;
        private RudpReliableOutbound reliableOutbound;

        public RudpInboundPump(
            UdpClient client,
            IPEndPoint expectedServer,
            ulong sessionId,
            ulong sessionGeneration)
        {
            this.client = client ?? throw new ArgumentNullException(nameof(client));
            this.expectedServer = expectedServer ?? throw new ArgumentNullException(nameof(expectedServer));
            if (sessionId == 0 || sessionGeneration == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(sessionId));
            }
            this.sessionId = sessionId;
            this.sessionGeneration = sessionGeneration;
        }

        public async Task<bool> ReceiveOnceAsync()
        {
            RunOwnership ownership = AcquireOwnership();
            try
            {
                return await ReceiveOnceCoreAsync().ConfigureAwait(false);
            }
            finally
            {
                Volatile.Write(ref ownership.Active, 0);
            }
        }

        private async Task<bool> ReceiveOnceCoreAsync()
        {
            UdpReceiveResult received = await client.ReceiveAsync().ConfigureAwait(false);
            if (!SameEndpoint(received.RemoteEndPoint, expectedServer))
            {
                return false;
            }

            try
            {
                RudpInboundDatagram datagram = RudpProtocolCodec.DecodeInbound(received.Buffer);
                if (datagram.Header.SessionId != sessionId ||
                    datagram.Header.SessionGeneration != sessionGeneration)
                {
                    return false;
                }
                bool admitted = true;
                RudpReliableOutbound outbound;
                lock (inboundGate)
                {
                    if (datagram.Message is RudpBindAccepted)
                    {
                        if (transportEpoch != 0 &&
                            datagram.Header.TransportEpoch == transportEpoch &&
                            rebindPrepared)
                        {
                            return false;
                        }
                        if (transportEpoch != 0 &&
                            datagram.Header.TransportEpoch != transportEpoch)
                        {
                            if (!rebindPrepared)
                            {
                                return false;
                            }
                            ResetForTransportEpoch();
                        }
                        transportEpoch = datagram.Header.TransportEpoch;
                        rebindPrepared = false;
                    }
                    else if (transportEpoch != 0 &&
                             datagram.Header.TransportEpoch != transportEpoch)
                    {
                        return false;
                    }
                    bool duplicate = datagram.Header.Flag == RudpFlag.Reliable &&
                                     RudpProtocolCodec.IsAcknowledged(
                                         datagram.Header.Sequence,
                                         receiveHistory.Ack,
                                         receiveHistory.AckBits);
                    if (!duplicate)
                    {
                        if (datagram.Message is RudpBindAccepted)
                        {
                            pendingControl = datagram;
                        }
                        else if (datagram.Message is RudpStateSnapshot)
                        {
                            latestMovementSnapshot = datagram;
                        }
                        else if (datagram.Message is RudpMonsterStateSnapshot)
                        {
                            latestCombatSnapshot = datagram;
                        }
                        else if (datagram.Message is RudpDropStateSnapshot)
                        {
                            latestLootSnapshot = datagram;
                        }
                        else if (datagram.Message is RudpClaimLootTerminalResult ||
                                 datagram.Message is RudpDropSpawned)
                        {
                            if (pendingLoot.Count >= LootReliableCapacity)
                            {
                                admitted = false;
                            }
                            else
                            {
                                pendingLoot.Enqueue(datagram);
                            }
                        }
                        else if (pendingCombat.Count < CombatReliableCapacity)
                        {
                            pendingCombat.Enqueue(datagram);
                        }
                        else
                        {
                            admitted = false;
                        }
                    }
                    if (admitted && !duplicate)
                    {
                        receiveHistory.Observe(datagram.Header.Sequence);
                    }
                    outbound = reliableOutbound;
                }
                outbound?.Acknowledge(datagram.Header.Ack, datagram.Header.AckBits);
                return admitted;
            }
            catch (RudpProtocolException)
            {
                return false;
            }
        }

        public async Task RunAsync(CancellationToken cancellationToken)
        {
            RunOwnership ownership = AcquireOwnership();
            try
            {
                using (cancellationToken.Register(client.Close))
                {
                    try
                    {
                        while (!cancellationToken.IsCancellationRequested)
                        {
                            await ReceiveOnceCoreAsync().ConfigureAwait(false);
                        }
                    }
                    catch (ObjectDisposedException) when (cancellationToken.IsCancellationRequested)
                    {
                    }
                    catch (SocketException) when (cancellationToken.IsCancellationRequested)
                    {
                    }
                }
            }
            finally
            {
                Volatile.Write(ref ownership.Active, 0);
            }
        }

        public bool TryDequeue(out RudpInboundDatagram datagram)
        {
            lock (inboundGate)
            {
                if (TryTakeControl(out datagram) || TryTakeCombat(out datagram) ||
                    TryTakeLoot(out datagram) || TryTakeMovementSnapshot(out datagram) ||
                    TryTakeCombatSnapshot(out datagram) || TryTakeLootSnapshot(out datagram))
                {
                    return true;
                }
                datagram = null;
                return false;
            }
        }

        public void PrepareForRebind()
        {
            lock (inboundGate)
            {
                rebindPrepared = true;
            }
        }

        internal void AttachReliableOutbound(RudpReliableOutbound outbound)
        {
            if (outbound == null)
            {
                throw new ArgumentNullException(nameof(outbound));
            }
            lock (inboundGate)
            {
                if (reliableOutbound != null &&
                    !ReferenceEquals(reliableOutbound, outbound))
                {
                    throw new InvalidOperationException(
                        "RUDP session already has a reliable outbound owner");
                }
                reliableOutbound = outbound;
            }
        }

        public bool TryDequeueMovement(out RudpInboundDatagram datagram)
        {
            lock (inboundGate)
            {
                if (TryTakeControl(out datagram) || TryTakeMovementSnapshot(out datagram))
                {
                    return true;
                }
                datagram = null;
                return false;
            }
        }

        public bool TryDequeueCombat(out RudpInboundDatagram datagram)
        {
            lock (inboundGate)
            {
                if (TryTakeCombat(out datagram) || TryTakeCombatSnapshot(out datagram))
                {
                    return true;
                }
                datagram = null;
                return false;
            }
        }

        public bool TryDequeueLoot(out RudpInboundDatagram datagram)
        {
            lock (inboundGate)
            {
                if (TryTakeLoot(out datagram) || TryTakeLootSnapshot(out datagram))
                {
                    return true;
                }
                datagram = null;
                return false;
            }
        }

        public void ObserveAccepted(RudpInboundDatagram datagram)
        {
            if (datagram == null || datagram.Header.SessionId != sessionId ||
                datagram.Header.SessionGeneration != sessionGeneration)
            {
                throw new ArgumentException("Accepted datagram identity does not match the pump", nameof(datagram));
            }
            lock (inboundGate)
            {
                receiveHistory.Observe(datagram.Header.Sequence);
            }
        }

        public RudpHeader CreateOutboundHeader(
            RudpFlag flag,
            uint transportEpoch,
            uint sequence,
            ushort messageId)
        {
            lock (inboundGate)
            {
                return new RudpHeader(
                    flag,
                    sessionId,
                    sessionGeneration,
                    transportEpoch,
                    sequence,
                    receiveHistory.Ack,
                    receiveHistory.AckBits,
                    messageId);
            }
        }

        private void ResetForTransportEpoch()
        {
            receiveHistory.Reset();
            pendingCombat.Clear();
            pendingLoot.Clear();
            pendingControl = null;
            latestMovementSnapshot = null;
            latestCombatSnapshot = null;
            latestLootSnapshot = null;
        }

        private bool TryTakeControl(out RudpInboundDatagram datagram)
        {
            datagram = pendingControl;
            pendingControl = null;
            return datagram != null;
        }

        private bool TryTakeCombat(out RudpInboundDatagram datagram)
        {
            if (pendingCombat.Count != 0)
            {
                datagram = pendingCombat.Dequeue();
                return true;
            }
            datagram = null;
            return false;
        }

        private bool TryTakeMovementSnapshot(out RudpInboundDatagram datagram)
        {
            datagram = latestMovementSnapshot;
            latestMovementSnapshot = null;
            return datagram != null;
        }

        private bool TryTakeLoot(out RudpInboundDatagram datagram)
        {
            if (pendingLoot.Count != 0)
            {
                datagram = pendingLoot.Dequeue();
                return true;
            }
            datagram = null;
            return false;
        }

        private bool TryTakeCombatSnapshot(out RudpInboundDatagram datagram)
        {
            datagram = latestCombatSnapshot;
            latestCombatSnapshot = null;
            return datagram != null;
        }

        private bool TryTakeLootSnapshot(out RudpInboundDatagram datagram)
        {
            datagram = latestLootSnapshot;
            latestLootSnapshot = null;
            return datagram != null;
        }

        private static bool SameEndpoint(IPEndPoint left, IPEndPoint right)
        {
            return left.Port == right.Port && left.Address.Equals(right.Address);
        }

        private RunOwnership AcquireOwnership()
        {
            RunOwnership ownership = Ownership.GetOrCreateValue(client);
            if (Interlocked.CompareExchange(ref ownership.Active, 1, 0) != 0)
            {
                throw new InvalidOperationException(
                    "UDP peer already has an active inbound pump");
            }
            return ownership;
        }

        private sealed class RunOwnership
        {
            public int Active;
        }
    }
}
