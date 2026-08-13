using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Text;

namespace LootOfLegends.Protocol
{
    public enum RoomWireResultCode : ushort
    {
        Ok = 0,
        InvalidArgument = 1,
        AlreadyInRoom = 2,
        RoomNotFound = 3,
        RoomClosed = 4,
        RoomFull = 5,
        NotInRoom = 6,
        NotHost = 7,
        NotEnoughPlayers = 8,
        NotAllReady = 9,
        InvalidTarget = 10,
        RoomOverloaded = 11,
        StaleSession = 12
    }

    public abstract class LobbyRoomServerMessage
    {
    }

    public sealed class RoomSummary
    {
        public RoomSummary(ulong roomId, string title, byte memberCount, byte capacity)
        {
            RoomId = roomId;
            Title = title;
            MemberCount = memberCount;
            Capacity = capacity;
        }

        public ulong RoomId { get; }
        public string Title { get; }
        public byte MemberCount { get; }
        public byte Capacity { get; }
    }

    public sealed class RoomMember
    {
        public RoomMember(
            ulong sessionId,
            ulong sessionGeneration,
            string nickname,
            bool ready)
        {
            if (sessionId == 0 || sessionGeneration == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(sessionId));
            }
            if (string.IsNullOrEmpty(nickname))
            {
                throw new ArgumentException("Room member nickname cannot be empty", nameof(nickname));
            }
            SessionId = sessionId;
            SessionGeneration = sessionGeneration;
            Nickname = nickname;
            Ready = ready;
        }

        public ulong SessionId { get; }
        public ulong SessionGeneration { get; }
        public string Nickname { get; }
        public bool Ready { get; }
    }

    public sealed class LobbyEntrySnapshot : LobbyRoomServerMessage
    {
        private readonly IReadOnlyList<RoomSummary> rooms;

        public LobbyEntrySnapshot(
            ulong sessionId,
            ulong sessionGeneration,
            string nickname,
            IReadOnlyList<RoomSummary> rooms)
        {
            SessionId = sessionId;
            SessionGeneration = sessionGeneration;
            Nickname = nickname;
            this.rooms = Copy(rooms);
        }

        public ulong SessionId { get; }
        public ulong SessionGeneration { get; }
        public string Nickname { get; }
        public IReadOnlyList<RoomSummary> Rooms => rooms;

        private static IReadOnlyList<RoomSummary> Copy(IReadOnlyList<RoomSummary> source)
        {
            return new ReadOnlyCollection<RoomSummary>(
                new List<RoomSummary>(source ?? throw new ArgumentNullException(nameof(source))));
        }
    }

    public sealed class LobbyRoomListUpdate : LobbyRoomServerMessage
    {
        private readonly IReadOnlyList<RoomSummary> rooms;

        public LobbyRoomListUpdate(IReadOnlyList<RoomSummary> rooms)
        {
            this.rooms = new ReadOnlyCollection<RoomSummary>(
                new List<RoomSummary>(rooms ?? throw new ArgumentNullException(nameof(rooms))));
        }

        public IReadOnlyList<RoomSummary> Rooms => rooms;
    }

    public sealed class RoomCommandResponse : LobbyRoomServerMessage
    {
        public RoomCommandResponse(ulong requestId, ushort resultCode)
        {
            RequestId = requestId;
            ResultCode = resultCode;
        }

        public ulong RequestId { get; }
        public ushort ResultCode { get; }
        public RoomWireResultCode Result => (RoomWireResultCode)ResultCode;
    }

    public sealed class RoomDetailProjection : LobbyRoomServerMessage
    {
        private readonly IReadOnlyList<RoomMember> members;

        public RoomDetailProjection(
            ulong roomId,
            string title,
            byte capacity,
            ulong hostSessionId,
            ulong hostSessionGeneration,
            IReadOnlyList<RoomMember> members)
        {
            if (roomId == 0 || capacity < 2 || capacity > 10 ||
                hostSessionId == 0 || hostSessionGeneration == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(roomId));
            }
            if (string.IsNullOrEmpty(title))
            {
                throw new ArgumentException("Room title cannot be empty", nameof(title));
            }
            if (members == null || members.Count == 0 || members.Count > capacity)
            {
                throw new ArgumentException("Room members are outside capacity", nameof(members));
            }
            bool hostPresent = false;
            foreach (RoomMember member in members)
            {
                if (member == null)
                {
                    throw new ArgumentException("Room member cannot be null", nameof(members));
                }
                hostPresent |= member.SessionId == hostSessionId &&
                               member.SessionGeneration == hostSessionGeneration;
            }
            if (!hostPresent)
            {
                throw new ArgumentException("Room host must be present", nameof(members));
            }

            RoomId = roomId;
            Title = title;
            Capacity = capacity;
            HostSessionId = hostSessionId;
            HostSessionGeneration = hostSessionGeneration;
            this.members = new ReadOnlyCollection<RoomMember>(new List<RoomMember>(members));
        }

        public ulong RoomId { get; }
        public string Title { get; }
        public byte Capacity { get; }
        public ulong HostSessionId { get; }
        public ulong HostSessionGeneration { get; }
        public IReadOnlyList<RoomMember> Members => members;
    }

    public sealed class LobbyRoomProtocolException : Exception
    {
        public LobbyRoomProtocolException(string message)
            : base(message)
        {
        }

        public LobbyRoomProtocolException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    public static class LobbyRoomProtocolCodec
    {
        private const byte ProtocolMajor = 1;
        private const uint LobbyEntrySnapshotId = 5;
        private const uint LobbyRoomListUpdateId = 6;
        private const uint RoomCommandResponseId = 12;
        private const uint RoomDetailProjectionId = 13;
        private const int MaximumRoomTitleBytes = 48;
        private const int MaximumMembers = 10;
        private const ushort MaximumResultCode = 12;
        private static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);

        public static LobbyRoomServerMessage DecodeServerFrame(byte[] frame)
        {
            if (frame == null || frame.Length < 9)
            {
                throw new LobbyRoomProtocolException("Lobby/Room frame is partial");
            }
            uint payloadLength = ReadUInt32(frame, 0);
            if (payloadLength != frame.Length - 4)
            {
                throw new LobbyRoomProtocolException("Lobby/Room frame length is invalid");
            }
            if (frame[4] != ProtocolMajor)
            {
                throw new LobbyRoomProtocolException("Unsupported Lobby/Room protocol version");
            }

            switch (ReadUInt32(frame, 5))
            {
                case LobbyEntrySnapshotId:
                    return DecodeEntry(frame);
                case LobbyRoomListUpdateId:
                    return DecodeListUpdate(frame);
                case RoomCommandResponseId:
                    return DecodeResponse(frame);
                case RoomDetailProjectionId:
                    return DecodeDetail(frame);
                default:
                    throw new LobbyRoomProtocolException("Unsupported Lobby/Room message");
            }
        }

        public static byte[] EncodeCreateRoom(ulong requestId, string title, byte capacity)
        {
            if (requestId == 0 || capacity < 2 || capacity > MaximumMembers)
            {
                throw new ArgumentOutOfRangeException(nameof(requestId));
            }
            byte[] titleBytes = NormalizeTitle(title);
            var body = new byte[8 + 1 + titleBytes.Length + 1];
            WriteUInt64(body, 0, requestId);
            body[8] = (byte)titleBytes.Length;
            Buffer.BlockCopy(titleBytes, 0, body, 9, titleBytes.Length);
            body[body.Length - 1] = capacity;
            return EncodeClientFrame(7, body);
        }

        public static byte[] EncodeJoinRoom(ulong requestId, ulong roomId)
        {
            if (requestId == 0 || roomId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(requestId));
            }
            var body = new byte[16];
            WriteUInt64(body, 0, requestId);
            WriteUInt64(body, 8, roomId);
            return EncodeClientFrame(8, body);
        }

        public static byte[] EncodeLeaveRoom(ulong requestId)
        {
            return EncodeRequestOnly(9, requestId);
        }

        public static byte[] EncodeSetReady(ulong requestId, bool ready)
        {
            if (requestId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(requestId));
            }
            var body = new byte[9];
            WriteUInt64(body, 0, requestId);
            body[8] = ready ? (byte)1 : (byte)0;
            return EncodeClientFrame(10, body);
        }

        public static byte[] EncodeKickRoomMember(
            ulong requestId,
            ulong targetSessionId,
            ulong targetSessionGeneration)
        {
            if (requestId == 0 || targetSessionId == 0 || targetSessionGeneration == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(requestId));
            }
            var body = new byte[24];
            WriteUInt64(body, 0, requestId);
            WriteUInt64(body, 8, targetSessionId);
            WriteUInt64(body, 16, targetSessionGeneration);
            return EncodeClientFrame(11, body);
        }

        private static byte[] EncodeRequestOnly(uint messageId, ulong requestId)
        {
            if (requestId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(requestId));
            }
            var body = new byte[8];
            WriteUInt64(body, 0, requestId);
            return EncodeClientFrame(messageId, body);
        }

        private static byte[] EncodeClientFrame(uint messageId, byte[] body)
        {
            uint payloadLength = checked((uint)(1 + 4 + body.Length));
            var frame = new byte[4 + payloadLength];
            WriteUInt32(frame, 0, payloadLength);
            frame[4] = ProtocolMajor;
            WriteUInt32(frame, 5, messageId);
            Buffer.BlockCopy(body, 0, frame, 9, body.Length);
            return frame;
        }

        private static byte[] NormalizeTitle(string title)
        {
            if (title == null)
            {
                throw new ArgumentNullException(nameof(title));
            }
            string normalized = title.Trim(' ', '\t', '\n', '\r', '\f', '\v');
            foreach (char character in normalized)
            {
                if (character <= '\u001f' ||
                    (character >= '\u007f' && character <= '\u009f'))
                {
                    throw new ArgumentException("Room title contains a control character", nameof(title));
                }
            }
            byte[] bytes;
            try
            {
                bytes = StrictUtf8.GetBytes(normalized);
            }
            catch (EncoderFallbackException error)
            {
                throw new ArgumentException("Room title is not valid UTF-8", nameof(title), error);
            }
            if (bytes.Length == 0 || bytes.Length > MaximumRoomTitleBytes)
            {
                throw new ArgumentException("Room title length is invalid", nameof(title));
            }
            return bytes;
        }

        private static LobbyEntrySnapshot DecodeEntry(byte[] frame)
        {
            RequireAvailable(frame, 9, 18);
            ulong sessionId = ReadUInt64(frame, 9);
            ulong generation = ReadUInt64(frame, 17);
            int nicknameLength = ReadUInt16(frame, 25);
            int offset = 27;
            string nickname = ReadText(frame, ref offset, nicknameLength, "Lobby nickname");
            IReadOnlyList<RoomSummary> rooms = DecodeRoomList(frame, offset, out int end);
            if (sessionId == 0 || generation == 0 || nicknameLength == 0 || end != frame.Length)
            {
                throw new LobbyRoomProtocolException("Malformed lobby entry");
            }
            return new LobbyEntrySnapshot(sessionId, generation, nickname, rooms);
        }

        private static RoomCommandResponse DecodeResponse(byte[] frame)
        {
            RequireLength(frame, 19);
            ulong requestId = ReadUInt64(frame, 9);
            ushort resultCode = ReadUInt16(frame, 17);
            if (requestId == 0 || resultCode > MaximumResultCode)
            {
                throw new LobbyRoomProtocolException("Malformed room command response");
            }
            return new RoomCommandResponse(requestId, resultCode);
        }

        private static LobbyRoomListUpdate DecodeListUpdate(byte[] frame)
        {
            IReadOnlyList<RoomSummary> rooms = DecodeRoomList(frame, 9, out int end);
            if (end != frame.Length)
            {
                throw new LobbyRoomProtocolException("Room list has trailing bytes");
            }
            return new LobbyRoomListUpdate(rooms);
        }

        private static RoomDetailProjection DecodeDetail(byte[] frame)
        {
            RequireAvailable(frame, 9, 9);
            ulong roomId = ReadUInt64(frame, 9);
            int offset = 17;
            int titleLength = frame[offset++];
            string title = ReadText(frame, ref offset, titleLength, "Room title");
            RequireAvailable(frame, offset, 18);
            byte capacity = frame[offset++];
            ulong hostSessionId = ReadUInt64(frame, offset);
            offset += 8;
            ulong hostGeneration = ReadUInt64(frame, offset);
            offset += 8;
            int memberCount = frame[offset++];
            if (roomId == 0 || titleLength == 0 || titleLength > MaximumRoomTitleBytes ||
                capacity < 2 || capacity > MaximumMembers || memberCount == 0 ||
                memberCount > capacity)
            {
                throw new LobbyRoomProtocolException("Malformed room detail prefix");
            }
            var members = new List<RoomMember>(memberCount);
            for (int index = 0; index < memberCount; index++)
            {
                members.Add(DecodeMember(frame, ref offset));
            }
            if (offset != frame.Length)
            {
                throw new LobbyRoomProtocolException("Room detail has trailing bytes");
            }
            try
            {
                return new RoomDetailProjection(
                    roomId, title, capacity, hostSessionId, hostGeneration, members);
            }
            catch (ArgumentException error)
            {
                throw new LobbyRoomProtocolException("Malformed room detail", error);
            }
        }

        private static IReadOnlyList<RoomSummary> DecodeRoomList(
            byte[] frame,
            int offset,
            out int end)
        {
            RequireAvailable(frame, offset, 2);
            int count = ReadUInt16(frame, offset);
            offset += 2;
            var rooms = new List<RoomSummary>(count);
            for (int index = 0; index < count; index++)
            {
                RequireAvailable(frame, offset, 9);
                ulong roomId = ReadUInt64(frame, offset);
                offset += 8;
                int titleLength = frame[offset++];
                string title = ReadText(frame, ref offset, titleLength, "Room title");
                RequireAvailable(frame, offset, 2);
                byte memberCount = frame[offset++];
                byte capacity = frame[offset++];
                if (roomId == 0 || titleLength == 0 || titleLength > MaximumRoomTitleBytes ||
                    memberCount == 0 || memberCount > capacity || capacity < 2 ||
                    capacity > MaximumMembers)
                {
                    throw new LobbyRoomProtocolException("Malformed room summary");
                }
                rooms.Add(new RoomSummary(roomId, title, memberCount, capacity));
            }
            end = offset;
            return new ReadOnlyCollection<RoomSummary>(rooms);
        }

        private static RoomMember DecodeMember(byte[] frame, ref int offset)
        {
            RequireAvailable(frame, offset, 18);
            ulong sessionId = ReadUInt64(frame, offset);
            offset += 8;
            ulong generation = ReadUInt64(frame, offset);
            offset += 8;
            int nicknameLength = ReadUInt16(frame, offset);
            offset += 2;
            string nickname = ReadText(frame, ref offset, nicknameLength, "Room member nickname");
            RequireAvailable(frame, offset, 1);
            byte ready = frame[offset++];
            if (sessionId == 0 || generation == 0 || nicknameLength == 0 || ready > 1)
            {
                throw new LobbyRoomProtocolException("Malformed room member");
            }
            try
            {
                return new RoomMember(sessionId, generation, nickname, ready == 1);
            }
            catch (ArgumentException error)
            {
                throw new LobbyRoomProtocolException("Malformed room member", error);
            }
        }

        private static string ReadText(byte[] frame, ref int offset, int length, string field)
        {
            RequireAvailable(frame, offset, length);
            try
            {
                string result = StrictUtf8.GetString(frame, offset, length);
                offset += length;
                return result;
            }
            catch (DecoderFallbackException error)
            {
                throw new LobbyRoomProtocolException(field + " is not UTF-8", error);
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

        private static void RequireLength(byte[] frame, int expected)
        {
            if (frame.Length != expected)
            {
                throw new LobbyRoomProtocolException("Unexpected Lobby/Room payload length");
            }
        }

        private static void RequireAvailable(byte[] bytes, int offset, int count)
        {
            if (count < 0 || offset < 0 || offset > bytes.Length - count)
            {
                throw new LobbyRoomProtocolException("Lobby/Room payload is truncated");
            }
        }
    }
}
