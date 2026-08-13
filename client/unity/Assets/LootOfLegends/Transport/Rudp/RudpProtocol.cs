using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace LootOfLegends.Transport.Rudp
{
    public enum RudpFlag : byte
    {
        Unreliable = 0,
        Reliable = 1,
        AckOnly = 2,
        Heartbeat = 4
    }

    public sealed class RudpHeader
    {
        public RudpHeader(
            RudpFlag flag,
            ulong sessionId,
            ulong sessionGeneration,
            uint transportEpoch,
            uint sequence,
            uint ack,
            uint ackBits,
            ushort messageId)
        {
            Flag = flag;
            SessionId = sessionId;
            SessionGeneration = sessionGeneration;
            TransportEpoch = transportEpoch;
            Sequence = sequence;
            Ack = ack;
            AckBits = ackBits;
            MessageId = messageId;
        }

        public RudpFlag Flag { get; }
        public ulong SessionId { get; }
        public ulong SessionGeneration { get; }
        public uint TransportEpoch { get; }
        public uint Sequence { get; }
        public uint Ack { get; }
        public uint AckBits { get; }
        public ushort MessageId { get; }
    }

    public sealed class RudpBindCapability
    {
        private readonly byte[] bytes;

        internal RudpBindCapability(ulong requestId, uint ttlMillis, byte[] bytes)
        {
            RequestId = requestId;
            TtlMillis = ttlMillis;
            this.bytes = (byte[])bytes.Clone();
        }

        public ulong RequestId { get; }
        public uint TtlMillis { get; }
        public byte[] Bytes => (byte[])bytes.Clone();
    }

    public sealed class RudpBindAccepted
    {
    }

    public sealed class RudpSnapshotPlayer
    {
        public RudpSnapshotPlayer(
            ulong sessionId,
            int positionXMillimeters,
            int positionYMillimeters)
        {
            if (sessionId == 0 ||
                positionXMillimeters < RudpProtocolCodec.MinimumPositionMillimeters ||
                positionXMillimeters > RudpProtocolCodec.MaximumPositionMillimeters ||
                positionYMillimeters < RudpProtocolCodec.MinimumPositionMillimeters ||
                positionYMillimeters > RudpProtocolCodec.MaximumPositionMillimeters)
            {
                throw new ArgumentOutOfRangeException(nameof(sessionId));
            }
            SessionId = sessionId;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
        }

        public ulong SessionId { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
    }

    public sealed class RudpStateSnapshot
    {
        private readonly IReadOnlyList<RudpSnapshotPlayer> players;

        public RudpStateSnapshot(
            ulong battleInstanceId,
            uint snapshotSequence,
            uint serverTick,
            IEnumerable<RudpSnapshotPlayer> players)
        {
            if (battleInstanceId == 0 || players == null)
            {
                throw new ArgumentException("Snapshot correlation and players are required");
            }

            var copy = new List<RudpSnapshotPlayer>(players);
            if (copy.Count > RudpProtocolCodec.MaximumPlayers)
            {
                throw new ArgumentOutOfRangeException(nameof(players));
            }
            var sessionIds = new HashSet<ulong>();
            foreach (RudpSnapshotPlayer player in copy)
            {
                if (player == null || !sessionIds.Add(player.SessionId))
                {
                    throw new ArgumentException("Snapshot players must be unique", nameof(players));
                }
            }

            BattleInstanceId = battleInstanceId;
            SnapshotSequence = snapshotSequence;
            ServerTick = serverTick;
            this.players = new ReadOnlyCollection<RudpSnapshotPlayer>(copy);
        }

        public ulong BattleInstanceId { get; }
        public uint SnapshotSequence { get; }
        public uint ServerTick { get; }
        public IReadOnlyList<RudpSnapshotPlayer> Players => players;
    }

    public sealed class RudpCommandId
    {
        public RudpCommandId(ulong high, ulong low)
        {
            if (high == 0 && low == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(high));
            }
            High = high;
            Low = low;
        }

        public ulong High { get; }
        public ulong Low { get; }

        public static RudpCommandId Create()
        {
            byte[] bytes = Guid.NewGuid().ToByteArray();
            return new RudpCommandId(
                BitConverter.ToUInt64(bytes, 0),
                BitConverter.ToUInt64(bytes, 8));
        }
    }

    public sealed class RudpEventId
    {
        public RudpEventId(ulong high, ulong low)
        {
            if (high == 0 && low == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(high));
            }
            High = high;
            Low = low;
        }

        public ulong High { get; }
        public ulong Low { get; }
    }

    public enum RudpAttackResultCode : ushort
    {
        Ok = 0,
        NotEligible = 1,
        StaleSession = 2,
        StaleBattle = 3,
        InvalidTarget = 4,
        OutOfRange = 5,
        Cooldown = 6,
        Overloaded = 7,
        CommandConflict = 8,
        TerminalAlreadyDecided = 9
    }

    public enum RudpCombatOutcome : byte
    {
        None = 0,
        MonsterDefeated = 1,
        CombatTimeout = 2
    }

    public enum RudpMonsterState : byte
    {
        Alive = 0,
        Dying = 1,
        Dead = 2,
        TimedOut = 3
    }

    public enum RudpEventStreamKind : byte
    {
        CombatLifecycle = 1,
        LootLifecycle = 2
    }

    public sealed class RudpAttackTerminalResult
    {
        public RudpAttackTerminalResult(
            RudpCommandId commandId,
            ulong battleInstanceId,
            RudpAttackResultCode resultCode,
            ulong monsterId,
            uint remainingHitPoints,
            ushort rulesetVersion,
            RudpCombatOutcome combatOutcome)
        {
            CommandId = commandId;
            BattleInstanceId = battleInstanceId;
            ResultCode = resultCode;
            MonsterId = monsterId;
            RemainingHitPoints = remainingHitPoints;
            RulesetVersion = rulesetVersion;
            CombatOutcome = combatOutcome;
            if (!RudpProtocolCodec.IsValid(this))
            {
                throw new ArgumentException("Invalid attack terminal result");
            }
        }

        public RudpCommandId CommandId { get; }
        public ulong BattleInstanceId { get; }
        public RudpAttackResultCode ResultCode { get; }
        public ulong MonsterId { get; }
        public uint RemainingHitPoints { get; }
        public ushort RulesetVersion { get; }
        public RudpCombatOutcome CombatOutcome { get; }
    }

    public sealed class RudpMonsterSpawned
    {
        public RudpMonsterSpawned(
            RudpEventId eventId,
            ulong battleInstanceId,
            RudpEventStreamKind eventStreamKind,
            uint eventSequence,
            ulong monsterId,
            int positionXMillimeters,
            int positionYMillimeters,
            uint maximumHitPoints,
            ushort rulesetVersion)
        {
            EventId = eventId;
            BattleInstanceId = battleInstanceId;
            EventStreamKind = eventStreamKind;
            EventSequence = eventSequence;
            MonsterId = monsterId;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
            MaximumHitPoints = maximumHitPoints;
            RulesetVersion = rulesetVersion;
            if (!RudpProtocolCodec.IsValid(this))
            {
                throw new ArgumentException("Invalid monster spawned event");
            }
        }

        public RudpEventId EventId { get; }
        public ulong BattleInstanceId { get; }
        public RudpEventStreamKind EventStreamKind { get; }
        public uint EventSequence { get; }
        public ulong MonsterId { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
        public uint MaximumHitPoints { get; }
        public ushort RulesetVersion { get; }
    }

    public sealed class RudpCombatTerminalEvent
    {
        public RudpCombatTerminalEvent(
            RudpEventId eventId,
            ulong battleInstanceId,
            RudpEventStreamKind eventStreamKind,
            uint eventSequence,
            RudpCombatOutcome combatOutcome,
            ulong monsterId,
            uint serverTick,
            ushort rulesetVersion)
        {
            EventId = eventId;
            BattleInstanceId = battleInstanceId;
            EventStreamKind = eventStreamKind;
            EventSequence = eventSequence;
            CombatOutcome = combatOutcome;
            MonsterId = monsterId;
            ServerTick = serverTick;
            RulesetVersion = rulesetVersion;
            if (!RudpProtocolCodec.IsValid(this))
            {
                throw new ArgumentException("Invalid combat terminal event");
            }
        }

        public RudpEventId EventId { get; }
        public ulong BattleInstanceId { get; }
        public RudpEventStreamKind EventStreamKind { get; }
        public uint EventSequence { get; }
        public RudpCombatOutcome CombatOutcome { get; }
        public ulong MonsterId { get; }
        public uint ServerTick { get; }
        public ushort RulesetVersion { get; }
    }

    public sealed class RudpMonsterStateSnapshot
    {
        public RudpMonsterStateSnapshot(
            ulong battleInstanceId,
            uint snapshotSequence,
            uint serverTick,
            ulong monsterId,
            uint hitPoints,
            RudpMonsterState monsterState)
        {
            BattleInstanceId = battleInstanceId;
            SnapshotSequence = snapshotSequence;
            ServerTick = serverTick;
            MonsterId = monsterId;
            HitPoints = hitPoints;
            MonsterState = monsterState;
            if (!RudpProtocolCodec.IsValid(this))
            {
                throw new ArgumentException("Invalid monster state snapshot");
            }
        }

        public ulong BattleInstanceId { get; }
        public uint SnapshotSequence { get; }
        public uint ServerTick { get; }
        public ulong MonsterId { get; }
        public uint HitPoints { get; }
        public RudpMonsterState MonsterState { get; }
    }

    public enum RudpClaimLootResultCode : ushort
    {
        Ok = 0,
        NotEligible = 1,
        StaleSession = 2,
        StaleBattle = 3,
        InvalidDrop = 4,
        UnknownDrop = 5,
        OutOfRange = 6,
        AlreadyClaimed = 7,
        Overloaded = 8,
        CommandConflict = 9,
        CatalogRejected = 10,
        ResolutionClosed = 11
    }

    public enum RudpLootResolutionState : byte
    {
        NotStarted = 0,
        Open = 1,
        Resolved = 2
    }

    public enum RudpLootDropState : byte
    {
        Available = 0,
        Claimed = 1,
        Unclaimed = 2
    }

    public sealed class RudpClaimLootIntent
    {
        public RudpClaimLootIntent(
            RudpCommandId commandId,
            ulong battleInstanceId,
            ulong dropId)
        {
            CommandId = commandId ?? throw new ArgumentNullException(nameof(commandId));
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            BattleInstanceId = battleInstanceId;
            DropId = dropId;
        }

        public RudpCommandId CommandId { get; }
        public ulong BattleInstanceId { get; }
        public ulong DropId { get; }
    }

    public sealed class RudpClaimLootTerminalResult
    {
        public RudpClaimLootTerminalResult(
            RudpCommandId commandId,
            ulong battleInstanceId,
            ulong dropId,
            RudpClaimLootResultCode resultCode)
        {
            CommandId = commandId;
            BattleInstanceId = battleInstanceId;
            DropId = dropId;
            ResultCode = resultCode;
            if (!RudpProtocolCodec.IsValid(this))
            {
                throw new ArgumentException("Invalid claim loot terminal result");
            }
        }

        public RudpCommandId CommandId { get; }
        public ulong BattleInstanceId { get; }
        public ulong DropId { get; }
        public RudpClaimLootResultCode ResultCode { get; }
    }

    public sealed class RudpDropSpawned
    {
        public RudpDropSpawned(
            RudpEventId eventId,
            ulong battleInstanceId,
            RudpEventStreamKind eventStreamKind,
            uint eventSequence,
            ulong dropId,
            ulong itemId,
            ulong quantity,
            int positionXMillimeters,
            int positionYMillimeters,
            ushort rulesetVersion)
        {
            EventId = eventId;
            BattleInstanceId = battleInstanceId;
            EventStreamKind = eventStreamKind;
            EventSequence = eventSequence;
            DropId = dropId;
            ItemId = itemId;
            Quantity = quantity;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
            RulesetVersion = rulesetVersion;
            if (!RudpProtocolCodec.IsValid(this))
            {
                throw new ArgumentException("Invalid drop spawned event");
            }
        }

        public RudpEventId EventId { get; }
        public ulong BattleInstanceId { get; }
        public RudpEventStreamKind EventStreamKind { get; }
        public uint EventSequence { get; }
        public ulong DropId { get; }
        public ulong ItemId { get; }
        public ulong Quantity { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
        public ushort RulesetVersion { get; }
    }

    public sealed class RudpLootDropProjection
    {
        public RudpLootDropProjection(
            ulong dropId,
            ulong itemId,
            ulong quantity,
            int positionXMillimeters,
            int positionYMillimeters,
            RudpLootDropState state,
            ulong ownerSessionId)
        {
            DropId = dropId;
            ItemId = itemId;
            Quantity = quantity;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
            State = state;
            OwnerSessionId = ownerSessionId;
            if (!RudpProtocolCodec.IsValid(this))
            {
                throw new ArgumentException("Invalid loot drop projection");
            }
        }

        public ulong DropId { get; }
        public ulong ItemId { get; }
        public ulong Quantity { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
        public RudpLootDropState State { get; }
        public ulong OwnerSessionId { get; }
    }

    public sealed class RudpDropStateSnapshot
    {
        private readonly IReadOnlyList<RudpLootDropProjection> drops;

        public RudpDropStateSnapshot(
            ulong battleInstanceId,
            uint snapshotSequence,
            RudpLootResolutionState resolutionState,
            IEnumerable<RudpLootDropProjection> drops)
        {
            if (drops == null)
            {
                throw new ArgumentNullException(nameof(drops));
            }
            BattleInstanceId = battleInstanceId;
            SnapshotSequence = snapshotSequence;
            ResolutionState = resolutionState;
            this.drops = new ReadOnlyCollection<RudpLootDropProjection>(
                new List<RudpLootDropProjection>(drops));
            if (!RudpProtocolCodec.IsValid(this))
            {
                throw new ArgumentException("Invalid drop state snapshot");
            }
        }

        public ulong BattleInstanceId { get; }
        public uint SnapshotSequence { get; }
        public RudpLootResolutionState ResolutionState { get; }
        public IReadOnlyList<RudpLootDropProjection> Drops => drops;
    }

    public sealed class RudpInboundDatagram
    {
        internal RudpInboundDatagram(RudpHeader header, object message)
        {
            Header = header;
            Message = message;
        }

        public RudpHeader Header { get; }
        public object Message { get; }
    }

    public sealed class RudpProtocolException : Exception
    {
        public RudpProtocolException(string message)
            : base(message)
        {
        }
    }

    public sealed class RudpReceiveHistory
    {
        public uint Ack { get; private set; }
        public uint AckBits { get; private set; }

        public void Reset()
        {
            Ack = 0;
            AckBits = 0;
        }

        public void Observe(uint sequence)
        {
            if (sequence == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(sequence));
            }
            if (Ack == 0)
            {
                Ack = sequence;
                return;
            }
            if (sequence == Ack)
            {
                return;
            }
            if (RudpProtocolCodec.IsSequenceNewer(sequence, Ack))
            {
                uint distance = sequence - Ack;
                AckBits = distance > 32
                    ? 0
                    : (distance == 32 ? 0 : AckBits << (int)distance) |
                      (1u << (int)(distance - 1));
                Ack = sequence;
                return;
            }

            uint olderDistance = Ack - sequence;
            if (olderDistance <= 32)
            {
                AckBits |= 1u << (int)(olderDistance - 1);
            }
        }
    }

    public static class RudpProtocolCodec
    {
        public const int MinimumPositionMillimeters = -10000;
        public const int MaximumPositionMillimeters = 10000;
        public const int MaximumPlayers = 10;
        public const int MaximumDrops = 10;

        private const uint Magic = 0x4c4f4c32;
        private const byte ProtocolMajor = 1;
        private const int HeaderBytes = 48;
        private const int MaximumDatagramBytes = 1200;
        private const int CrcOffset = 44;
        private const uint BindCapabilityTtlMillis = 15000;
        private const ushort BindHelloMessageId = 22;
        private const ushort BindAcceptedMessageId = 23;
        private const ushort HeartbeatMessageId = 24;
        private const ushort MoveIntentMessageId = 25;
        private const ushort StateSnapshotMessageId = 26;
        private const ushort AttackIntentMessageId = 27;
        private const ushort AttackTerminalResultMessageId = 28;
        private const ushort MonsterSpawnedMessageId = 29;
        private const ushort CombatTerminalEventMessageId = 30;
        private const ushort MonsterStateSnapshotMessageId = 31;
        private const ushort ClaimLootIntentMessageId = 32;
        private const ushort ClaimLootTerminalResultMessageId = 33;
        private const ushort DropSpawnedMessageId = 34;
        private const ushort DropStateSnapshotMessageId = 35;
        private const uint CombatMaximumHitPoints = 1600;
        private const uint CombatAttackDamage = 20;
        private const ushort CombatRulesetVersion = 1;
        private const uint CrcPolynomial = 0xedb88320;

        public static byte[] EncodeBindCapabilityRequest(ulong requestId)
        {
            if (requestId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(requestId));
            }
            var frame = new byte[17];
            WriteUInt32(frame, 0, 13);
            frame[4] = ProtocolMajor;
            WriteUInt32(frame, 5, 20);
            WriteUInt64(frame, 9, requestId);
            return frame;
        }

        public static RudpBindCapability DecodeBindCapability(byte[] frame)
        {
            if (frame == null || frame.Length != 53 || ReadUInt32(frame, 0) != 49 ||
                frame[4] != ProtocolMajor || ReadUInt32(frame, 5) != 21)
            {
                throw new RudpProtocolException("Malformed RUDP bind capability frame");
            }
            ulong requestId = ReadUInt64(frame, 9);
            uint ttlMillis = ReadUInt32(frame, 17);
            var capability = new byte[32];
            Buffer.BlockCopy(frame, 21, capability, 0, capability.Length);
            if (requestId == 0 || ttlMillis != BindCapabilityTtlMillis || AllZero(capability))
            {
                throw new RudpProtocolException("Invalid RUDP bind capability");
            }
            return new RudpBindCapability(requestId, ttlMillis, capability);
        }

        public static byte[] EncodeBindHello(RudpHeader header, byte[] capability)
        {
            if (header == null || header.Flag != RudpFlag.Reliable ||
                header.MessageId != BindHelloMessageId || header.TransportEpoch != 0 ||
                capability == null || capability.Length != 32 || AllZero(capability))
            {
                throw new ArgumentException("Invalid RUDP bind hello");
            }
            return Encode(header, capability);
        }

        public static byte[] EncodeHeartbeat(RudpHeader header)
        {
            if (header == null || header.Flag != RudpFlag.Heartbeat ||
                header.MessageId != HeartbeatMessageId || header.TransportEpoch == 0)
            {
                throw new ArgumentException("Invalid RUDP heartbeat");
            }
            return Encode(header, Array.Empty<byte>());
        }

        public static byte[] EncodeMoveIntent(
            RudpHeader header,
            ulong battleInstanceId,
            uint actionSequence,
            short desiredX,
            short desiredY)
        {
            if (header == null || header.Flag != RudpFlag.Unreliable ||
                header.MessageId != MoveIntentMessageId || header.TransportEpoch == 0 ||
                battleInstanceId == 0 || desiredX == short.MinValue || desiredY == short.MinValue)
            {
                throw new ArgumentException("Invalid RUDP movement intent");
            }
            var payload = new byte[18];
            WriteUInt64(payload, 0, battleInstanceId);
            WriteUInt32(payload, 8, actionSequence);
            WriteUInt16(payload, 12, unchecked((ushort)desiredX));
            WriteUInt16(payload, 14, unchecked((ushort)desiredY));
            WriteUInt16(payload, 16, 0);
            return Encode(header, payload);
        }

        public static byte[] EncodeAttackIntent(
            RudpHeader header,
            RudpCommandId commandId,
            ulong battleInstanceId,
            ulong targetHint)
        {
            if (header == null || header.Flag != RudpFlag.Reliable ||
                header.MessageId != AttackIntentMessageId || header.TransportEpoch == 0 ||
                commandId == null || battleInstanceId == 0 || targetHint == 0)
            {
                throw new ArgumentException("Invalid RUDP attack intent");
            }
            var payload = new byte[32];
            WriteUInt64(payload, 0, commandId.High);
            WriteUInt64(payload, 8, commandId.Low);
            WriteUInt64(payload, 16, battleInstanceId);
            WriteUInt64(payload, 24, targetHint);
            return Encode(header, payload);
        }

        public static byte[] EncodeClaimLootIntent(
            RudpHeader header,
            RudpCommandId commandId,
            ulong battleInstanceId,
            ulong dropId)
        {
            if (header == null || header.Flag != RudpFlag.Reliable ||
                header.MessageId != ClaimLootIntentMessageId ||
                header.TransportEpoch == 0 || commandId == null ||
                battleInstanceId == 0)
            {
                throw new ArgumentException("Invalid RUDP claim loot intent");
            }
            var payload = new byte[32];
            WriteUInt64(payload, 0, commandId.High);
            WriteUInt64(payload, 8, commandId.Low);
            WriteUInt64(payload, 16, battleInstanceId);
            WriteUInt64(payload, 24, dropId);
            return Encode(header, payload);
        }

        public static RudpInboundDatagram DecodeInbound(byte[] datagram)
        {
            WireDatagram wire = Decode(datagram);
            try
            {
                if (wire.Header.MessageId == BindAcceptedMessageId)
                {
                    if (wire.Header.Flag != RudpFlag.Reliable ||
                        wire.Header.TransportEpoch == 0 || wire.Payload.Length != 0)
                    {
                        throw new RudpProtocolException("Malformed RUDP bind accepted datagram");
                    }
                    return new RudpInboundDatagram(wire.Header, new RudpBindAccepted());
                }
                if (wire.Header.MessageId == StateSnapshotMessageId &&
                    wire.Header.Flag == RudpFlag.Unreliable && wire.Header.TransportEpoch != 0)
                {
                    return new RudpInboundDatagram(wire.Header, DecodeSnapshot(wire.Payload));
                }
                if (wire.Header.MessageId == AttackTerminalResultMessageId &&
                    wire.Header.Flag == RudpFlag.Reliable && wire.Header.TransportEpoch != 0 &&
                    wire.Payload.Length == 41)
                {
                    return new RudpInboundDatagram(wire.Header, new RudpAttackTerminalResult(
                        new RudpCommandId(
                            ReadUInt64(wire.Payload, 0), ReadUInt64(wire.Payload, 8)),
                        ReadUInt64(wire.Payload, 16),
                        (RudpAttackResultCode)ReadUInt16(wire.Payload, 24),
                        ReadUInt64(wire.Payload, 26),
                        ReadUInt32(wire.Payload, 34),
                        ReadUInt16(wire.Payload, 38),
                        (RudpCombatOutcome)wire.Payload[40]));
                }
                if (wire.Header.MessageId == MonsterSpawnedMessageId &&
                    wire.Header.Flag == RudpFlag.Reliable && wire.Header.TransportEpoch != 0 &&
                    wire.Payload.Length == 51)
                {
                    return new RudpInboundDatagram(wire.Header, new RudpMonsterSpawned(
                        new RudpEventId(
                            ReadUInt64(wire.Payload, 0), ReadUInt64(wire.Payload, 8)),
                        ReadUInt64(wire.Payload, 16),
                        (RudpEventStreamKind)wire.Payload[24],
                        ReadUInt32(wire.Payload, 25),
                        ReadUInt64(wire.Payload, 29),
                        unchecked((int)ReadUInt32(wire.Payload, 37)),
                        unchecked((int)ReadUInt32(wire.Payload, 41)),
                        ReadUInt32(wire.Payload, 45),
                        ReadUInt16(wire.Payload, 49)));
                }
                if (wire.Header.MessageId == CombatTerminalEventMessageId &&
                    wire.Header.Flag == RudpFlag.Reliable && wire.Header.TransportEpoch != 0 &&
                    wire.Payload.Length == 44)
                {
                    return new RudpInboundDatagram(wire.Header, new RudpCombatTerminalEvent(
                        new RudpEventId(
                            ReadUInt64(wire.Payload, 0), ReadUInt64(wire.Payload, 8)),
                        ReadUInt64(wire.Payload, 16),
                        (RudpEventStreamKind)wire.Payload[24],
                        ReadUInt32(wire.Payload, 25),
                        (RudpCombatOutcome)wire.Payload[29],
                        ReadUInt64(wire.Payload, 30),
                        ReadUInt32(wire.Payload, 38),
                        ReadUInt16(wire.Payload, 42)));
                }
                if (wire.Header.MessageId == MonsterStateSnapshotMessageId &&
                    wire.Header.Flag == RudpFlag.Unreliable && wire.Header.TransportEpoch != 0 &&
                    wire.Payload.Length == 29)
                {
                    return new RudpInboundDatagram(wire.Header, new RudpMonsterStateSnapshot(
                        ReadUInt64(wire.Payload, 0),
                        ReadUInt32(wire.Payload, 8),
                        ReadUInt32(wire.Payload, 12),
                        ReadUInt64(wire.Payload, 16),
                        ReadUInt32(wire.Payload, 24),
                        (RudpMonsterState)wire.Payload[28]));
                }
                if (wire.Header.MessageId == ClaimLootTerminalResultMessageId &&
                    wire.Header.Flag == RudpFlag.Reliable &&
                    wire.Header.TransportEpoch != 0 && wire.Payload.Length == 34)
                {
                    return new RudpInboundDatagram(
                        wire.Header,
                        new RudpClaimLootTerminalResult(
                            new RudpCommandId(
                                ReadUInt64(wire.Payload, 0),
                                ReadUInt64(wire.Payload, 8)),
                            ReadUInt64(wire.Payload, 16),
                            ReadUInt64(wire.Payload, 24),
                            (RudpClaimLootResultCode)ReadUInt16(wire.Payload, 32)));
                }
                if (wire.Header.MessageId == DropSpawnedMessageId &&
                    wire.Header.Flag == RudpFlag.Reliable &&
                    wire.Header.TransportEpoch != 0 && wire.Payload.Length == 63)
                {
                    return new RudpInboundDatagram(
                        wire.Header,
                        new RudpDropSpawned(
                            new RudpEventId(
                                ReadUInt64(wire.Payload, 0),
                                ReadUInt64(wire.Payload, 8)),
                            ReadUInt64(wire.Payload, 16),
                            (RudpEventStreamKind)wire.Payload[24],
                            ReadUInt32(wire.Payload, 25),
                            ReadUInt64(wire.Payload, 29),
                            ReadUInt64(wire.Payload, 37),
                            ReadUInt64(wire.Payload, 45),
                            unchecked((int)ReadUInt32(wire.Payload, 53)),
                            unchecked((int)ReadUInt32(wire.Payload, 57)),
                            ReadUInt16(wire.Payload, 61)));
                }
                if (wire.Header.MessageId == DropStateSnapshotMessageId &&
                    wire.Header.Flag == RudpFlag.Unreliable &&
                    wire.Header.TransportEpoch != 0)
                {
                    return new RudpInboundDatagram(
                        wire.Header, DecodeLootSnapshot(wire.Payload));
                }
            }
            catch (ArgumentException error)
            {
                throw new RudpProtocolException("Malformed combat message: " + error.Message);
            }
            throw new RudpProtocolException("Unsupported RUDP inbound message");
        }

        internal static bool IsValid(RudpAttackTerminalResult message)
        {
            return message.CommandId != null && message.BattleInstanceId != 0 &&
                   (ushort)message.ResultCode <= (ushort)RudpAttackResultCode.TerminalAlreadyDecided &&
                   message.MonsterId == 1 && ValidCombatHitPoints(message.RemainingHitPoints) &&
                   message.RulesetVersion == CombatRulesetVersion &&
                   (byte)message.CombatOutcome <= (byte)RudpCombatOutcome.CombatTimeout &&
                   (message.CombatOutcome != RudpCombatOutcome.MonsterDefeated ||
                    message.RemainingHitPoints == 0);
        }

        internal static bool IsValid(RudpMonsterSpawned message)
        {
            return message.EventId != null && message.BattleInstanceId != 0 &&
                   message.EventStreamKind == RudpEventStreamKind.CombatLifecycle &&
                   message.EventSequence == 1 && message.MonsterId == 1 &&
                   message.PositionXMillimeters == 0 && message.PositionYMillimeters == 0 &&
                   message.MaximumHitPoints == CombatMaximumHitPoints &&
                   message.RulesetVersion == CombatRulesetVersion;
        }

        internal static bool IsValid(RudpCombatTerminalEvent message)
        {
            return message.EventId != null && message.BattleInstanceId != 0 &&
                   message.EventStreamKind == RudpEventStreamKind.CombatLifecycle &&
                   message.EventSequence == 2 &&
                   (message.CombatOutcome == RudpCombatOutcome.MonsterDefeated ||
                    message.CombatOutcome == RudpCombatOutcome.CombatTimeout) &&
                   message.MonsterId == 1 &&
                   message.RulesetVersion == CombatRulesetVersion;
        }

        internal static bool IsValid(RudpMonsterStateSnapshot message)
        {
            if (message.BattleInstanceId == 0 || message.SnapshotSequence == 0 ||
                message.MonsterId != 1 || !ValidCombatHitPoints(message.HitPoints))
            {
                return false;
            }
            switch (message.MonsterState)
            {
                case RudpMonsterState.Alive:
                    return message.HitPoints > 0;
                case RudpMonsterState.Dying:
                case RudpMonsterState.Dead:
                    return message.HitPoints == 0;
                case RudpMonsterState.TimedOut:
                    return message.HitPoints > 0;
                default:
                    return false;
            }
        }

        internal static bool IsValid(RudpClaimLootTerminalResult message)
        {
            return message.CommandId != null && message.BattleInstanceId != 0 &&
                   (ushort)message.ResultCode <=
                   (ushort)RudpClaimLootResultCode.ResolutionClosed &&
                   (message.ResultCode != RudpClaimLootResultCode.Ok ||
                    message.DropId != 0);
        }

        internal static bool IsValid(RudpDropSpawned message)
        {
            return message.EventId != null && message.BattleInstanceId != 0 &&
                   message.EventStreamKind == RudpEventStreamKind.LootLifecycle &&
                   message.EventSequence != 0 && message.DropId == message.EventSequence &&
                   message.DropId <= MaximumDrops && ValidLootItem(message.ItemId) &&
                   message.Quantity == 1 && ValidPosition(message.PositionXMillimeters) &&
                   ValidPosition(message.PositionYMillimeters) &&
                   message.RulesetVersion == 1;
        }

        internal static bool IsValid(RudpLootDropProjection drop)
        {
            if (drop == null || drop.DropId == 0 || drop.DropId > MaximumDrops ||
                !ValidLootItem(drop.ItemId) || drop.Quantity != 1 ||
                !ValidPosition(drop.PositionXMillimeters) ||
                !ValidPosition(drop.PositionYMillimeters))
            {
                return false;
            }
            switch (drop.State)
            {
                case RudpLootDropState.Available:
                case RudpLootDropState.Unclaimed:
                    return drop.OwnerSessionId == 0;
                case RudpLootDropState.Claimed:
                    return drop.OwnerSessionId != 0;
                default:
                    return false;
            }
        }

        internal static bool IsValid(RudpDropStateSnapshot snapshot)
        {
            if (snapshot.BattleInstanceId == 0 || snapshot.SnapshotSequence == 0 ||
                snapshot.Drops.Count > MaximumDrops)
            {
                return false;
            }
            if (snapshot.ResolutionState == RudpLootResolutionState.NotStarted)
            {
                return snapshot.Drops.Count == 0;
            }
            if (snapshot.Drops.Count == 0 ||
                (snapshot.ResolutionState != RudpLootResolutionState.Open &&
                 snapshot.ResolutionState != RudpLootResolutionState.Resolved))
            {
                return false;
            }
            var dropIds = new HashSet<ulong>();
            bool hasAvailable = false;
            foreach (RudpLootDropProjection drop in snapshot.Drops)
            {
                if (!IsValid(drop) || !dropIds.Add(drop.DropId))
                {
                    return false;
                }
                hasAvailable |= drop.State == RudpLootDropState.Available;
            }
            return (snapshot.ResolutionState == RudpLootResolutionState.Open) ==
                   hasAvailable;
        }

        public static bool IsSequenceNewer(uint candidate, uint reference)
        {
            return candidate != reference && unchecked(candidate - reference) < 0x80000000u;
        }

        public static bool IsAcknowledged(uint sequence, uint ack, uint ackBits)
        {
            if (sequence == 0 || ack == 0)
            {
                return false;
            }
            if (sequence == ack)
            {
                return true;
            }
            if (IsSequenceNewer(sequence, ack))
            {
                return false;
            }
            uint distance = unchecked(ack - sequence);
            return distance >= 1 && distance <= 32 &&
                   (ackBits & (1u << (int)(distance - 1))) != 0;
        }

        private static RudpStateSnapshot DecodeSnapshot(byte[] payload)
        {
            if (payload.Length < 18)
            {
                throw new RudpProtocolException("State snapshot payload is partial");
            }
            ulong battleInstanceId = ReadUInt64(payload, 0);
            uint snapshotSequence = ReadUInt32(payload, 8);
            uint serverTick = ReadUInt32(payload, 12);
            ushort playerCount = ReadUInt16(payload, 16);
            if (battleInstanceId == 0 || playerCount > MaximumPlayers ||
                payload.Length != 18 + (playerCount * 16))
            {
                throw new RudpProtocolException("Malformed state snapshot payload");
            }

            var players = new List<RudpSnapshotPlayer>(playerCount);
            try
            {
                for (int index = 0; index < playerCount; index++)
                {
                    int offset = 18 + (index * 16);
                    players.Add(new RudpSnapshotPlayer(
                        ReadUInt64(payload, offset),
                        unchecked((int)ReadUInt32(payload, offset + 8)),
                        unchecked((int)ReadUInt32(payload, offset + 12))));
                }
                return new RudpStateSnapshot(
                    battleInstanceId, snapshotSequence, serverTick, players);
            }
            catch (ArgumentException error)
            {
                throw new RudpProtocolException("Invalid state snapshot player: " + error.Message);
            }
        }

        private static RudpDropStateSnapshot DecodeLootSnapshot(byte[] payload)
        {
            if (payload.Length < 15)
            {
                throw new RudpProtocolException("Drop state snapshot payload is partial");
            }
            ulong battleInstanceId = ReadUInt64(payload, 0);
            uint snapshotSequence = ReadUInt32(payload, 8);
            var resolution = (RudpLootResolutionState)payload[12];
            ushort dropCount = ReadUInt16(payload, 13);
            if (dropCount > MaximumDrops || payload.Length != 15 + (dropCount * 41))
            {
                throw new RudpProtocolException("Malformed drop state snapshot payload");
            }
            var drops = new List<RudpLootDropProjection>(dropCount);
            for (int index = 0; index < dropCount; index++)
            {
                int offset = 15 + (index * 41);
                drops.Add(new RudpLootDropProjection(
                    ReadUInt64(payload, offset),
                    ReadUInt64(payload, offset + 8),
                    ReadUInt64(payload, offset + 16),
                    unchecked((int)ReadUInt32(payload, offset + 24)),
                    unchecked((int)ReadUInt32(payload, offset + 28)),
                    (RudpLootDropState)payload[offset + 32],
                    ReadUInt64(payload, offset + 33)));
            }
            return new RudpDropStateSnapshot(
                battleInstanceId, snapshotSequence, resolution, drops);
        }

        private static byte[] Encode(RudpHeader header, byte[] payload)
        {
            if (payload.Length > MaximumDatagramBytes - HeaderBytes || !ValidHeader(header, payload.Length))
            {
                throw new ArgumentException("Invalid RUDP header or payload");
            }
            var datagram = new byte[HeaderBytes + payload.Length];
            WriteUInt32(datagram, 0, Magic);
            datagram[4] = ProtocolMajor;
            datagram[5] = (byte)header.Flag;
            WriteUInt16(datagram, 6, HeaderBytes);
            WriteUInt64(datagram, 8, header.SessionId);
            WriteUInt64(datagram, 16, header.SessionGeneration);
            WriteUInt32(datagram, 24, header.TransportEpoch);
            WriteUInt32(datagram, 28, header.Sequence);
            WriteUInt32(datagram, 32, header.Ack);
            WriteUInt32(datagram, 36, header.AckBits);
            WriteUInt16(datagram, 40, header.MessageId);
            WriteUInt16(datagram, 42, payload.Length);
            Buffer.BlockCopy(payload, 0, datagram, HeaderBytes, payload.Length);
            WriteUInt32(datagram, CrcOffset, Checksum(datagram));
            return datagram;
        }

        private static WireDatagram Decode(byte[] datagram)
        {
            if (datagram == null || datagram.Length < HeaderBytes ||
                datagram.Length > MaximumDatagramBytes)
            {
                throw new RudpProtocolException("RUDP datagram length is outside bounds");
            }
            if (ReadUInt32(datagram, 0) != Magic || datagram[4] != ProtocolMajor ||
                ReadUInt16(datagram, 6) != HeaderBytes)
            {
                throw new RudpProtocolException("Unsupported RUDP envelope");
            }
            RudpFlag flag;
            switch (datagram[5])
            {
                case 0: flag = RudpFlag.Unreliable; break;
                case 1: flag = RudpFlag.Reliable; break;
                case 2: flag = RudpFlag.AckOnly; break;
                case 4: flag = RudpFlag.Heartbeat; break;
                default: throw new RudpProtocolException("Invalid RUDP flags");
            }
            int payloadBytes = ReadUInt16(datagram, 42);
            if (payloadBytes != datagram.Length - HeaderBytes ||
                ReadUInt32(datagram, CrcOffset) != Checksum(datagram))
            {
                throw new RudpProtocolException("RUDP length or checksum mismatch");
            }
            var header = new RudpHeader(
                flag,
                ReadUInt64(datagram, 8),
                ReadUInt64(datagram, 16),
                ReadUInt32(datagram, 24),
                ReadUInt32(datagram, 28),
                ReadUInt32(datagram, 32),
                ReadUInt32(datagram, 36),
                ReadUInt16(datagram, 40));
            if (!ValidHeader(header, payloadBytes))
            {
                throw new RudpProtocolException("Invalid RUDP header fields");
            }
            var payload = new byte[payloadBytes];
            Buffer.BlockCopy(datagram, HeaderBytes, payload, 0, payloadBytes);
            return new WireDatagram(header, payload);
        }

        private static bool ValidHeader(RudpHeader header, int payloadBytes)
        {
            if (header.SessionId == 0 || header.SessionGeneration == 0 || header.Sequence == 0)
            {
                return false;
            }
            switch (header.Flag)
            {
                case RudpFlag.Unreliable:
                case RudpFlag.Reliable:
                    return header.MessageId != 0;
                case RudpFlag.AckOnly:
                    return header.MessageId == 0 && payloadBytes == 0;
                case RudpFlag.Heartbeat:
                    return header.MessageId == HeartbeatMessageId && payloadBytes == 0;
                default:
                    return false;
            }
        }

        private static uint Checksum(byte[] datagram)
        {
            uint value = uint.MaxValue;
            for (int index = 0; index < datagram.Length; index++)
            {
                byte current = index >= CrcOffset && index < CrcOffset + 4
                    ? (byte)0
                    : datagram[index];
                value ^= current;
                for (int bit = 0; bit < 8; bit++)
                {
                    value = (value >> 1) ^ ((value & 1) != 0 ? CrcPolynomial : 0);
                }
            }
            return value ^ uint.MaxValue;
        }

        private static bool AllZero(byte[] bytes)
        {
            foreach (byte value in bytes)
            {
                if (value != 0)
                {
                    return false;
                }
            }
            return true;
        }

        private static bool ValidCombatHitPoints(uint hitPoints)
        {
            return hitPoints <= CombatMaximumHitPoints && hitPoints % CombatAttackDamage == 0;
        }

        private static bool ValidLootItem(ulong itemId)
        {
            return itemId == 1 || itemId == 2;
        }

        private static bool ValidPosition(int millimeters)
        {
            return millimeters >= MinimumPositionMillimeters &&
                   millimeters <= MaximumPositionMillimeters;
        }

        private static ushort ReadUInt16(byte[] bytes, int offset)
        {
            return (ushort)((bytes[offset] << 8) | bytes[offset + 1]);
        }

        private static uint ReadUInt32(byte[] bytes, int offset)
        {
            return ((uint)bytes[offset] << 24) |
                   ((uint)bytes[offset + 1] << 16) |
                   ((uint)bytes[offset + 2] << 8) |
                   bytes[offset + 3];
        }

        private static ulong ReadUInt64(byte[] bytes, int offset)
        {
            ulong value = 0;
            for (int index = 0; index < 8; index++)
            {
                value = (value << 8) | bytes[offset + index];
            }
            return value;
        }

        private static void WriteUInt16(byte[] bytes, int offset, int value)
        {
            bytes[offset] = (byte)(value >> 8);
            bytes[offset + 1] = (byte)value;
        }

        private static void WriteUInt32(byte[] bytes, int offset, uint value)
        {
            bytes[offset] = (byte)(value >> 24);
            bytes[offset + 1] = (byte)(value >> 16);
            bytes[offset + 2] = (byte)(value >> 8);
            bytes[offset + 3] = (byte)value;
        }

        private static void WriteUInt64(byte[] bytes, int offset, ulong value)
        {
            for (int index = 0; index < 8; index++)
            {
                bytes[offset + index] = (byte)(value >> ((7 - index) * 8));
            }
        }

        private sealed class WireDatagram
        {
            public WireDatagram(RudpHeader header, byte[] payload)
            {
                Header = header;
                Payload = payload;
            }

            public RudpHeader Header { get; }
            public byte[] Payload { get; }
        }
    }
}
