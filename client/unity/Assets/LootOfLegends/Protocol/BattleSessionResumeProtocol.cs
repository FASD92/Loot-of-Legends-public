using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Text;

namespace LootOfLegends.Protocol
{
    public enum BattleResumePhase : byte
    {
        Combat = 1,
        Loot = 2,
        Result = 3
    }

    public sealed class BattleResumePlayerState
    {
        public BattleResumePlayerState(
            ulong sessionId,
            int positionXMillimeters,
            int positionYMillimeters,
            bool healthKnown,
            uint hitPoints,
            uint maximumHitPoints,
            bool isAlive)
        {
            if (sessionId == 0 || (!healthKnown && (hitPoints != 0 || maximumHitPoints != 0)))
            {
                throw new ArgumentOutOfRangeException(nameof(sessionId));
            }
            if (healthKnown && (maximumHitPoints == 0 || hitPoints > maximumHitPoints))
            {
                throw new ArgumentException("Player hit points exceed maximum", nameof(hitPoints));
            }
            SessionId = sessionId;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
            HealthKnown = healthKnown;
            HitPoints = hitPoints;
            MaximumHitPoints = maximumHitPoints;
            IsAlive = isAlive;
        }

        public ulong SessionId { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
        public bool HealthKnown { get; }
        public uint HitPoints { get; }
        public uint MaximumHitPoints { get; }
        public bool IsAlive { get; }
    }

    public sealed class BattleResumeMonsterState
    {
        public const byte Alive = 0;
        public const byte Dying = 1;
        public const byte Dead = 2;
        public const byte TimedOut = 3;

        public BattleResumeMonsterState(
            ulong monsterId,
            int positionXMillimeters,
            int positionYMillimeters,
            uint hitPoints,
            uint maximumHitPoints,
            byte state)
        {
            if (monsterId == 0 || maximumHitPoints == 0 || hitPoints > maximumHitPoints ||
                state > TimedOut)
            {
                throw new ArgumentOutOfRangeException(nameof(monsterId));
            }
            MonsterId = monsterId;
            PositionXMillimeters = positionXMillimeters;
            PositionYMillimeters = positionYMillimeters;
            HitPoints = hitPoints;
            MaximumHitPoints = maximumHitPoints;
            State = state;
        }

        public ulong MonsterId { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
        public uint HitPoints { get; }
        public uint MaximumHitPoints { get; }
        public byte State { get; }
    }

    public sealed class BattleResumeDropState
    {
        public BattleResumeDropState(
            ulong dropId,
            ulong itemId,
            ulong quantity,
            int positionXMillimeters,
            int positionYMillimeters,
            byte state,
            ulong ownerSessionId)
        {
            if (dropId == 0 || itemId == 0 || quantity == 0 || state > 2 ||
                (state == 1) != (ownerSessionId != 0))
            {
                throw new ArgumentOutOfRangeException(nameof(dropId));
            }
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
        public byte State { get; }
        public ulong OwnerSessionId { get; }
    }

    public sealed class BattleResumeResultState
    {
        public BattleResumeResultState(
            FinalResultOutcome outcome,
            IReadOnlyList<FinalResultEntry> entries)
        {
            if (entries == null || entries.Count < 2)
            {
                throw new ArgumentException("Resume result requires at least two entries", nameof(entries));
            }
            Outcome = outcome;
            Entries = new ReadOnlyCollection<FinalResultEntry>(
                new List<FinalResultEntry>(entries));
        }

        public FinalResultOutcome Outcome { get; }
        public IReadOnlyList<FinalResultEntry> Entries { get; }
    }

    public sealed class BattleResumeSnapshot
    {
        public BattleResumeSnapshot(
            ulong requestId,
            ulong snapshotId,
            ulong roomId,
            ulong battleInstanceId,
            ulong playerSessionId,
            ulong sessionGeneration,
            BattleResumePhase phase,
            uint remainingMilliseconds,
            uint serverTick,
            IReadOnlyList<BattleResumePlayerState> players,
            BattleResumeMonsterState monster,
            IReadOnlyList<BattleResumeDropState> drops,
            ulong score,
            BattleResumeResultState result)
        {
            if (requestId == 0 || snapshotId == 0 || roomId == 0 || battleInstanceId == 0 ||
                playerSessionId == 0 || sessionGeneration == 0 ||
                phase < BattleResumePhase.Combat || phase > BattleResumePhase.Result ||
                players == null || players.Count < 2 || players.Count > 10 ||
                drops == null || drops.Count > 10 ||
                (phase == BattleResumePhase.Result) != (result != null) ||
                (phase == BattleResumePhase.Result && remainingMilliseconds != 0))
            {
                throw new ArgumentException("Resume snapshot identity or bounds are invalid");
            }
            bool localPlayerFound = false;
            var sessions = new HashSet<ulong>();
            foreach (BattleResumePlayerState player in players)
            {
                if (player == null || !sessions.Add(player.SessionId))
                {
                    throw new ArgumentException("Resume snapshot players must be unique", nameof(players));
                }
                localPlayerFound |= player.SessionId == playerSessionId;
            }
            if (!localPlayerFound)
            {
                throw new ArgumentException("Resume snapshot does not contain the local player", nameof(players));
            }
            var dropIds = new HashSet<ulong>();
            foreach (BattleResumeDropState drop in drops)
            {
                if (drop == null || !dropIds.Add(drop.DropId))
                {
                    throw new ArgumentException("Resume snapshot drops must be unique", nameof(drops));
                }
            }
            if (result != null)
            {
                try
                {
                    _ = new BattleFinalResult(
                        roomId,
                        battleInstanceId,
                        result.Outcome,
                        result.Entries);
                }
                catch (ArgumentException error)
                {
                    throw new ArgumentException("Resume result is invalid", nameof(result), error);
                }
            }

            RequestId = requestId;
            SnapshotId = snapshotId;
            RoomId = roomId;
            BattleInstanceId = battleInstanceId;
            PlayerSessionId = playerSessionId;
            SessionGeneration = sessionGeneration;
            Phase = phase;
            RemainingMilliseconds = remainingMilliseconds;
            ServerTick = serverTick;
            Players = new ReadOnlyCollection<BattleResumePlayerState>(
                new List<BattleResumePlayerState>(players));
            Monster = monster;
            Drops = new ReadOnlyCollection<BattleResumeDropState>(
                new List<BattleResumeDropState>(drops));
            Score = score;
            Result = result;
        }

        public ulong RequestId { get; }
        public ulong SnapshotId { get; }
        public ulong RoomId { get; }
        public ulong BattleInstanceId { get; }
        public ulong PlayerSessionId { get; }
        public ulong SessionGeneration { get; }
        public BattleResumePhase Phase { get; }
        public uint RemainingMilliseconds { get; }
        public uint ServerTick { get; }
        public IReadOnlyList<BattleResumePlayerState> Players { get; }
        public BattleResumeMonsterState Monster { get; }
        public IReadOnlyList<BattleResumeDropState> Drops { get; }
        public ulong Score { get; }
        public BattleResumeResultState Result { get; }
        public bool HasResult => Result != null;
    }

    public sealed class BattleSessionResumeProtocolException : Exception
    {
        public BattleSessionResumeProtocolException(string message)
            : base(message)
        {
        }

        public BattleSessionResumeProtocolException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    public static class BattleSessionResumeProtocolCodec
    {
        public const int MaximumServerPayloadBytes = 655552;

        private const byte ProtocolMajor = 1;
        private const uint ResumeBattleSessionMessageId = 40;
        private const uint BattleResumeSnapshotMessageId = 39;
        private const uint SnapshotAppliedMessageId = 41;
        private const int CredentialBytes = 43;
        private static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);

        public static byte[] EncodeResumeBattleSession(
            ulong requestId,
            ulong previousSessionId,
            ulong previousSessionGeneration,
            string credential)
        {
            if (requestId == 0 || previousSessionId == 0 || previousSessionGeneration == 0 ||
                !IsBase64Url43(credential))
            {
                throw new ArgumentException("Invalid Battle resume request");
            }
            byte[] frame = CreateFrame(74, ResumeBattleSessionMessageId);
            WriteUInt64(frame, 9, requestId);
            WriteUInt64(frame, 17, previousSessionId);
            WriteUInt64(frame, 25, previousSessionGeneration);
            WriteUInt16(frame, 33, CredentialBytes);
            Encoding.ASCII.GetBytes(credential, 0, credential.Length, frame, 35);
            return frame;
        }

        public static byte[] EncodeSnapshotApplied(ulong snapshotId)
        {
            if (snapshotId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(snapshotId));
            }
            byte[] frame = CreateFrame(13, SnapshotAppliedMessageId);
            WriteUInt64(frame, 9, snapshotId);
            return frame;
        }

        public static BattleResumeSnapshot DecodeServerFrame(byte[] frame)
        {
            if (frame == null || frame.Length < 9)
            {
                throw new BattleSessionResumeProtocolException("Battle resume frame is partial");
            }
            uint payloadLength = ReadUInt32(frame, 0);
            if (payloadLength != frame.Length - 4 || payloadLength > MaximumServerPayloadBytes ||
                frame[4] != ProtocolMajor || ReadUInt32(frame, 5) != BattleResumeSnapshotMessageId)
            {
                throw new BattleSessionResumeProtocolException("Unsupported Battle resume envelope");
            }

            int offset = 9;
            ulong requestId = ReadUInt64(frame, ref offset);
            ulong snapshotId = ReadUInt64(frame, ref offset);
            ulong roomId = ReadUInt64(frame, ref offset);
            ulong battleId = ReadUInt64(frame, ref offset);
            ulong playerSessionId = ReadUInt64(frame, ref offset);
            ulong generation = ReadUInt64(frame, ref offset);
            BattleResumePhase phase = (BattleResumePhase)ReadByte(frame, ref offset);
            uint remaining = ReadUInt32(frame, ref offset);
            uint serverTick = ReadUInt32(frame, ref offset);
            int playerCount = ReadUInt16(frame, ref offset);
            if (playerCount < 2 || playerCount > 10)
            {
                throw new BattleSessionResumeProtocolException("Resume player count is invalid");
            }
            var players = new List<BattleResumePlayerState>(playerCount);
            for (int index = 0; index < playerCount; index++)
            {
                ulong sessionId = ReadUInt64(frame, ref offset);
                int positionX = ReadInt32(frame, ref offset);
                int positionY = ReadInt32(frame, ref offset);
                bool healthKnown = ReadFlag(frame, ref offset, "player health-known");
                uint hitPoints = ReadUInt32(frame, ref offset);
                uint maximumHitPoints = ReadUInt32(frame, ref offset);
                bool alive = ReadFlag(frame, ref offset, "player alive");
                try
                {
                    players.Add(new BattleResumePlayerState(
                        sessionId,
                        positionX,
                        positionY,
                        healthKnown,
                        hitPoints,
                        maximumHitPoints,
                        alive));
                }
                catch (ArgumentException error)
                {
                    throw new BattleSessionResumeProtocolException("Resume player is invalid", error);
                }
            }

            BattleResumeMonsterState monster = null;
            if (ReadFlag(frame, ref offset, "monster presence"))
            {
                try
                {
                    monster = new BattleResumeMonsterState(
                        ReadUInt64(frame, ref offset),
                        ReadInt32(frame, ref offset),
                        ReadInt32(frame, ref offset),
                        ReadUInt32(frame, ref offset),
                        ReadUInt32(frame, ref offset),
                        ReadByte(frame, ref offset));
                }
                catch (ArgumentException error)
                {
                    throw new BattleSessionResumeProtocolException("Resume monster is invalid", error);
                }
            }

            int dropCount = ReadUInt16(frame, ref offset);
            if (dropCount > 10)
            {
                throw new BattleSessionResumeProtocolException("Resume drop count is invalid");
            }
            var drops = new List<BattleResumeDropState>(dropCount);
            for (int index = 0; index < dropCount; index++)
            {
                try
                {
                    drops.Add(new BattleResumeDropState(
                        ReadUInt64(frame, ref offset),
                        ReadUInt64(frame, ref offset),
                        ReadUInt64(frame, ref offset),
                        ReadInt32(frame, ref offset),
                        ReadInt32(frame, ref offset),
                        ReadByte(frame, ref offset),
                        ReadUInt64(frame, ref offset)));
                }
                catch (ArgumentException error)
                {
                    throw new BattleSessionResumeProtocolException("Resume drop is invalid", error);
                }
            }

            ulong score = ReadUInt64(frame, ref offset);
            BattleResumeResultState result = null;
            if (ReadFlag(frame, ref offset, "result presence"))
            {
                FinalResultOutcome outcome = (FinalResultOutcome)ReadByte(frame, ref offset);
                int resultCount = ReadUInt16(frame, ref offset);
                if (resultCount < 2 || resultCount > 10)
                {
                    throw new BattleSessionResumeProtocolException("Resume result count is invalid");
                }
                var entries = new List<FinalResultEntry>(resultCount);
                for (int index = 0; index < resultCount; index++)
                {
                    ulong sessionId = ReadUInt64(frame, ref offset);
                    int nicknameLength = ReadUInt16(frame, ref offset);
                    RequireAvailable(frame, offset, nicknameLength);
                    string nickname;
                    try
                    {
                        nickname = StrictUtf8.GetString(frame, offset, nicknameLength);
                    }
                    catch (DecoderFallbackException error)
                    {
                        throw new BattleSessionResumeProtocolException("Resume result nickname is not UTF-8", error);
                    }
                    offset += nicknameLength;
                    FinalResultExitStatus exitStatus = (FinalResultExitStatus)ReadByte(frame, ref offset);
                    ulong assetValue = ReadUInt64(frame, ref offset);
                    uint rank = ReadUInt32(frame, ref offset);
                    bool isTop = ReadFlag(frame, ref offset, "result top");
                    try
                    {
                        entries.Add(new FinalResultEntry(
                            sessionId,
                            nickname,
                            exitStatus,
                            assetValue,
                            rank == 0 ? (uint?)null : rank,
                            isTop));
                    }
                    catch (ArgumentException error)
                    {
                        throw new BattleSessionResumeProtocolException("Resume result entry is invalid", error);
                    }
                }
                try
                {
                    result = new BattleResumeResultState(outcome, entries);
                }
                catch (ArgumentException error)
                {
                    throw new BattleSessionResumeProtocolException("Resume result is invalid", error);
                }
            }
            if (offset != frame.Length)
            {
                throw new BattleSessionResumeProtocolException("Resume snapshot has trailing bytes");
            }
            try
            {
                return new BattleResumeSnapshot(
                    requestId,
                    snapshotId,
                    roomId,
                    battleId,
                    playerSessionId,
                    generation,
                    phase,
                    remaining,
                    serverTick,
                    players,
                    monster,
                    drops,
                    score,
                    result);
            }
            catch (ArgumentException error)
            {
                throw new BattleSessionResumeProtocolException("Resume snapshot is invalid", error);
            }
        }

        private static byte[] CreateFrame(uint payloadLength, uint messageId)
        {
            var frame = new byte[payloadLength + 4];
            WriteUInt32(frame, 0, payloadLength);
            frame[4] = ProtocolMajor;
            WriteUInt32(frame, 5, messageId);
            return frame;
        }

        private static bool IsBase64Url43(string value)
        {
            if (value == null || value.Length != CredentialBytes)
            {
                return false;
            }
            foreach (char character in value)
            {
                if (!(character >= 'A' && character <= 'Z') &&
                    !(character >= 'a' && character <= 'z') &&
                    !(character >= '0' && character <= '9') &&
                    character != '-' && character != '_')
                {
                    return false;
                }
            }
            return true;
        }

        private static ushort ReadUInt16(byte[] bytes, ref int offset)
        {
            RequireAvailable(bytes, offset, 2);
            ushort value = (ushort)((bytes[offset] << 8) | bytes[offset + 1]);
            offset += 2;
            return value;
        }

        private static uint ReadUInt32(byte[] bytes, int offset)
        {
            RequireAvailable(bytes, offset, 4);
            return ((uint)bytes[offset] << 24) |
                   ((uint)bytes[offset + 1] << 16) |
                   ((uint)bytes[offset + 2] << 8) |
                   bytes[offset + 3];
        }

        private static uint ReadUInt32(byte[] bytes, ref int offset)
        {
            RequireAvailable(bytes, offset, 4);
            uint value = ((uint)bytes[offset] << 24) |
                         ((uint)bytes[offset + 1] << 16) |
                         ((uint)bytes[offset + 2] << 8) |
                         bytes[offset + 3];
            offset += 4;
            return value;
        }

        private static ulong ReadUInt64(byte[] bytes, ref int offset)
        {
            RequireAvailable(bytes, offset, 8);
            ulong value = 0;
            for (int index = 0; index < 8; index++)
            {
                value = (value << 8) | bytes[offset + index];
            }
            offset += 8;
            return value;
        }

        private static int ReadInt32(byte[] bytes, ref int offset)
        {
            return unchecked((int)ReadUInt32(bytes, ref offset));
        }

        private static byte ReadByte(byte[] bytes, ref int offset)
        {
            RequireAvailable(bytes, offset, 1);
            return bytes[offset++];
        }

        private static bool ReadFlag(byte[] bytes, ref int offset, string field)
        {
            byte value = ReadByte(bytes, ref offset);
            if (value > 1)
            {
                throw new BattleSessionResumeProtocolException(field + " flag is invalid");
            }
            return value == 1;
        }

        private static void RequireAvailable(byte[] bytes, int offset, int count)
        {
            if (count < 0 || offset < 0 || offset > bytes.Length - count)
            {
                throw new BattleSessionResumeProtocolException("Resume snapshot is truncated");
            }
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
            WriteUInt32(bytes, offset, (uint)(value >> 32));
            WriteUInt32(bytes, offset + 4, (uint)value);
        }
    }
}
