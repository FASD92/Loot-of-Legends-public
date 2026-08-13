using System;
using System.Collections.Generic;
using System.Text;

namespace LootOfLegends.Protocol
{
    public enum BattleLoadResultCode : ushort
    {
        Ok = 0,
        InvalidArgument = 1,
        RoomNotFound = 2,
        RoomNotOpen = 3,
        NotInRoom = 4,
        NotHost = 5,
        NotEnoughPlayers = 6,
        NotAllReady = 7,
        StartGateClosed = 8,
        StaleSession = 9,
        StaleBattle = 10,
        NotEligible = 11,
        Overloaded = 12
    }

    public enum ArenaLoadCancelReason : ushort
    {
        NotEnoughReady = 1
    }

    public abstract class BattleLoadServerMessage
    {
    }

    public sealed class BattleCommandResponse : BattleLoadServerMessage
    {
        public BattleCommandResponse(ulong requestId, BattleLoadResultCode result)
        {
            RequestId = requestId;
            Result = result;
        }

        public ulong RequestId { get; }
        public BattleLoadResultCode Result { get; }
    }

    public sealed class ArenaLoadEntry : BattleLoadServerMessage
    {
        public ArenaLoadEntry(ulong roomId, ulong battleInstanceId)
        {
            RoomId = roomId;
            BattleInstanceId = battleInstanceId;
        }

        public ulong RoomId { get; }
        public ulong BattleInstanceId { get; }
    }

    public sealed class BattleParticipant
    {
        public BattleParticipant(ulong sessionId, ulong sessionGeneration, string nickname)
        {
            SessionId = sessionId;
            SessionGeneration = sessionGeneration;
            Nickname = nickname;
        }

        public ulong SessionId { get; }
        public ulong SessionGeneration { get; }
        public string Nickname { get; }
    }

    public sealed class ArenaGameplayStart : BattleLoadServerMessage
    {
        public ArenaGameplayStart(
            ulong roomId,
            ulong battleInstanceId,
            IReadOnlyList<BattleParticipant> participants)
        {
            RoomId = roomId;
            BattleInstanceId = battleInstanceId;
            Participants = new List<BattleParticipant>(
                participants ?? throw new ArgumentNullException(nameof(participants))).AsReadOnly();
        }

        public ulong RoomId { get; }
        public ulong BattleInstanceId { get; }
        public IReadOnlyList<BattleParticipant> Participants { get; }
    }

    public sealed class ArenaLoadCancelled : BattleLoadServerMessage
    {
        public ArenaLoadCancelled(
            ulong roomId,
            ulong battleInstanceId,
            ArenaLoadCancelReason reason)
        {
            RoomId = roomId;
            BattleInstanceId = battleInstanceId;
            Reason = reason;
        }

        public ulong RoomId { get; }
        public ulong BattleInstanceId { get; }
        public ArenaLoadCancelReason Reason { get; }
    }

    public sealed class BattleLoadProtocolException : Exception
    {
        public BattleLoadProtocolException(string message)
            : base(message)
        {
        }

        public BattleLoadProtocolException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    public static class BattleLoadProtocolCodec
    {
        public const int MaximumServerPayloadBytes = 655552;

        private const byte ProtocolMajor = 1;
        private const uint HostStartRequestId = 14;
        private const uint BattleCommandResponseId = 15;
        private const uint ArenaLoadEntryId = 16;
        private const uint ArenaLoadCompleteId = 17;
        private const uint ArenaGameplayStartId = 18;
        private const uint ArenaLoadCancelledId = 19;
        private const int MinimumParticipants = 2;
        private const int MaximumParticipants = 10;
        private const ushort MaximumResultCode = 12;
        private static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);

        public static byte[] EncodeHostStart(ulong requestId)
        {
            RequireNonZero(requestId, nameof(requestId));
            byte[] frame = CreateFrame(13, HostStartRequestId);
            WriteUInt64(frame, 9, requestId);
            return frame;
        }

        public static byte[] EncodeArenaLoadComplete(
            ulong requestId,
            ulong roomId,
            ulong battleInstanceId)
        {
            RequireNonZero(requestId, nameof(requestId));
            RequireNonZero(roomId, nameof(roomId));
            RequireNonZero(battleInstanceId, nameof(battleInstanceId));
            byte[] frame = CreateFrame(29, ArenaLoadCompleteId);
            WriteUInt64(frame, 9, requestId);
            WriteUInt64(frame, 17, roomId);
            WriteUInt64(frame, 25, battleInstanceId);
            return frame;
        }

        public static BattleLoadServerMessage DecodeServerFrame(byte[] frame)
        {
            if (frame == null || frame.Length < 9)
            {
                throw new BattleLoadProtocolException("Battle load frame is partial");
            }

            uint payloadLength = ReadUInt32(frame, 0);
            if (payloadLength != frame.Length - 4)
            {
                throw new BattleLoadProtocolException("Battle load frame length does not match payload");
            }
            if (frame[4] != ProtocolMajor)
            {
                throw new BattleLoadProtocolException("Unsupported battle load protocol version");
            }

            uint messageId = ReadUInt32(frame, 5);
            switch (messageId)
            {
                case BattleCommandResponseId:
                    return DecodeResponse(frame);
                case ArenaLoadEntryId:
                    return DecodeEntry(frame);
                case ArenaGameplayStartId:
                    return DecodeGameplayStart(frame);
                case ArenaLoadCancelledId:
                    return DecodeCancelled(frame);
                case HostStartRequestId:
                case ArenaLoadCompleteId:
                    throw new BattleLoadProtocolException("Client message received from server");
                default:
                    throw new BattleLoadProtocolException("Unsupported battle load message");
            }
        }

        private static BattleCommandResponse DecodeResponse(byte[] frame)
        {
            RequireLength(frame, 19);
            ulong requestId = ReadUInt64(frame, 9);
            ushort result = ReadUInt16(frame, 17);
            if (requestId == 0 || result > MaximumResultCode)
            {
                throw new BattleLoadProtocolException("Malformed battle command response");
            }
            return new BattleCommandResponse(requestId, (BattleLoadResultCode)result);
        }

        private static ArenaLoadEntry DecodeEntry(byte[] frame)
        {
            RequireLength(frame, 25);
            ulong roomId = ReadUInt64(frame, 9);
            ulong battleId = ReadUInt64(frame, 17);
            RequireCorrelation(roomId, battleId);
            return new ArenaLoadEntry(roomId, battleId);
        }

        private static ArenaGameplayStart DecodeGameplayStart(byte[] frame)
        {
            if (frame.Length < 26)
            {
                throw new BattleLoadProtocolException("Malformed gameplay start payload");
            }
            ulong roomId = ReadUInt64(frame, 9);
            ulong battleId = ReadUInt64(frame, 17);
            RequireCorrelation(roomId, battleId);
            int count = frame[25];
            if (count < MinimumParticipants || count > MaximumParticipants)
            {
                throw new BattleLoadProtocolException("Invalid gameplay participant count");
            }

            int offset = 26;
            var participants = new List<BattleParticipant>(count);
            var sessionIds = new HashSet<ulong>();
            for (int index = 0; index < count; index++)
            {
                RequireAvailable(frame, offset, 18);
                ulong sessionId = ReadUInt64(frame, offset);
                ulong generation = ReadUInt64(frame, offset + 8);
                int nicknameLength = ReadUInt16(frame, offset + 16);
                offset += 18;
                RequireAvailable(frame, offset, nicknameLength);
                string nickname;
                try
                {
                    nickname = StrictUtf8.GetString(frame, offset, nicknameLength);
                }
                catch (DecoderFallbackException error)
                {
                    throw new BattleLoadProtocolException("Participant nickname is not UTF-8", error);
                }
                if (sessionId == 0 || generation == 0 || nicknameLength == 0 ||
                    !sessionIds.Add(sessionId))
                {
                    throw new BattleLoadProtocolException("Malformed gameplay participant");
                }
                participants.Add(new BattleParticipant(sessionId, generation, nickname));
                offset += nicknameLength;
            }
            if (offset != frame.Length)
            {
                throw new BattleLoadProtocolException("Gameplay start has trailing bytes");
            }
            return new ArenaGameplayStart(roomId, battleId, participants);
        }

        private static ArenaLoadCancelled DecodeCancelled(byte[] frame)
        {
            RequireLength(frame, 27);
            ulong roomId = ReadUInt64(frame, 9);
            ulong battleId = ReadUInt64(frame, 17);
            ushort reason = ReadUInt16(frame, 25);
            RequireCorrelation(roomId, battleId);
            if (reason != (ushort)ArenaLoadCancelReason.NotEnoughReady)
            {
                throw new BattleLoadProtocolException("Unsupported load cancellation reason");
            }
            return new ArenaLoadCancelled(roomId, battleId, (ArenaLoadCancelReason)reason);
        }

        private static byte[] CreateFrame(uint payloadLength, uint messageId)
        {
            var frame = new byte[payloadLength + 4];
            WriteUInt32(frame, 0, payloadLength);
            frame[4] = ProtocolMajor;
            WriteUInt32(frame, 5, messageId);
            return frame;
        }

        private static void RequireNonZero(ulong value, string name)
        {
            if (value == 0)
            {
                throw new ArgumentOutOfRangeException(name);
            }
        }

        private static void RequireCorrelation(ulong roomId, ulong battleId)
        {
            if (roomId == 0 || battleId == 0)
            {
                throw new BattleLoadProtocolException("Battle correlation must be non-zero");
            }
        }

        private static void RequireLength(byte[] frame, int expected)
        {
            if (frame.Length != expected)
            {
                throw new BattleLoadProtocolException("Unexpected battle load payload length");
            }
        }

        private static void RequireAvailable(byte[] frame, int offset, int count)
        {
            if (count < 0 || offset < 0 || offset > frame.Length - count)
            {
                throw new BattleLoadProtocolException("Battle load payload is truncated");
            }
        }

        private static ushort ReadUInt16(byte[] bytes, int offset)
        {
            RequireAvailable(bytes, offset, 2);
            return (ushort)((bytes[offset] << 8) | bytes[offset + 1]);
        }

        private static uint ReadUInt32(byte[] bytes, int offset)
        {
            RequireAvailable(bytes, offset, 4);
            return ((uint)bytes[offset] << 24) |
                   ((uint)bytes[offset + 1] << 16) |
                   ((uint)bytes[offset + 2] << 8) |
                   bytes[offset + 3];
        }

        private static ulong ReadUInt64(byte[] bytes, int offset)
        {
            RequireAvailable(bytes, offset, 8);
            ulong value = 0;
            for (int index = 0; index < 8; index++)
            {
                value = (value << 8) | bytes[offset + index];
            }
            return value;
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
            for (int index = 7; index >= 0; index--)
            {
                bytes[offset + index] = (byte)value;
                value >>= 8;
            }
        }
    }
}
