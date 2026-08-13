using System;
using System.Text;

namespace LootOfLegends.Protocol
{
    public enum AuthenticationRejectedReason : ushort
    {
        Invalid = 1,
        Expired = 2,
        AlreadyConsumed = 3,
        WrongAudience = 4,
        DependencyUnavailable = 5,
        PreAuthCommand = 6
    }

    public enum SessionReplacedReason : ushort
    {
        SameAccountLogin = 1
    }

    public abstract class SessionServerMessage
    {
    }

    public sealed class WelcomeSession : SessionServerMessage
    {
        public WelcomeSession(
            ulong requestId,
            ulong sessionId,
            ulong sessionGeneration,
            ulong serverTimeUnixMillis,
            string nickname)
        {
            if (requestId == 0 || sessionId == 0 || sessionGeneration == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(requestId));
            }
            if (string.IsNullOrEmpty(nickname))
            {
                throw new ArgumentException("Session nickname cannot be empty", nameof(nickname));
            }
            RequestId = requestId;
            SessionId = sessionId;
            SessionGeneration = sessionGeneration;
            ServerTimeUnixMillis = serverTimeUnixMillis;
            Nickname = nickname;
        }

        public ulong RequestId { get; }
        public ulong SessionId { get; }
        public ulong SessionGeneration { get; }
        public ulong ServerTimeUnixMillis { get; }
        public string Nickname { get; }
    }

    public sealed class AuthenticationRejectedSession : SessionServerMessage
    {
        public AuthenticationRejectedSession(
            ulong requestId,
            AuthenticationRejectedReason reason)
        {
            if (requestId == 0 || !SessionProtocolCodec.IsValid(reason))
            {
                throw new ArgumentOutOfRangeException(nameof(requestId));
            }
            RequestId = requestId;
            Reason = reason;
        }

        public ulong RequestId { get; }
        public AuthenticationRejectedReason Reason { get; }
    }

    public sealed class SessionReplaced : SessionServerMessage
    {
        public SessionReplaced(SessionReplacedReason reason)
        {
            if (reason != SessionReplacedReason.SameAccountLogin)
            {
                throw new ArgumentOutOfRangeException(nameof(reason));
            }
            Reason = reason;
        }

        public SessionReplacedReason Reason { get; }
    }

    public sealed class SessionProtocolException : Exception
    {
        public SessionProtocolException(string message)
            : base(message)
        {
        }

        public SessionProtocolException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    public static class SessionProtocolCodec
    {
        private const byte ProtocolMajor = 1;
        private const uint AuthenticateMessageId = 1;
        private const uint WelcomeMessageId = 2;
        private const uint RejectedMessageId = 3;
        private const uint ReplacedMessageId = 4;
        private const int CredentialBytes = 43;
        private static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);

        public static byte[] EncodeAuthenticateGameSession(
            ulong requestId,
            string credential)
        {
            if (requestId == 0 || !IsBase64Url43(credential))
            {
                throw new ArgumentException("Invalid game authentication request");
            }
            byte[] frame = CreateFrame(58, AuthenticateMessageId);
            WriteUInt64(frame, 9, requestId);
            WriteUInt16(frame, 17, CredentialBytes);
            Encoding.ASCII.GetBytes(credential, 0, credential.Length, frame, 19);
            return frame;
        }

        public static SessionServerMessage DecodeServerFrame(byte[] frame)
        {
            if (frame == null || frame.Length < 9)
            {
                throw new SessionProtocolException("Session frame is partial");
            }
            uint payloadLength = ReadUInt32(frame, 0);
            if (payloadLength != frame.Length - 4 || frame[4] != ProtocolMajor)
            {
                throw new SessionProtocolException("Unsupported session envelope");
            }
            switch (ReadUInt32(frame, 5))
            {
                case WelcomeMessageId:
                    return DecodeWelcome(frame);
                case RejectedMessageId:
                    return DecodeRejected(frame);
                case ReplacedMessageId:
                    return DecodeReplaced(frame);
                case AuthenticateMessageId:
                    throw new SessionProtocolException("Client session message received from server");
                default:
                    throw new SessionProtocolException("Unsupported session message");
            }
        }

        internal static bool IsValid(AuthenticationRejectedReason reason)
        {
            return reason >= AuthenticationRejectedReason.Invalid &&
                reason <= AuthenticationRejectedReason.PreAuthCommand;
        }

        private static WelcomeSession DecodeWelcome(byte[] frame)
        {
            if (frame.Length < 44)
            {
                throw new SessionProtocolException("Welcome frame is truncated");
            }
            ulong requestId = ReadUInt64(frame, 9);
            ulong sessionId = ReadUInt64(frame, 17);
            ulong generation = ReadUInt64(frame, 25);
            ulong serverTime = ReadUInt64(frame, 33);
            int nicknameLength = ReadUInt16(frame, 41);
            if (nicknameLength == 0 || frame.Length != 43 + nicknameLength)
            {
                throw new SessionProtocolException("Welcome nickname length is invalid");
            }
            string nickname;
            try
            {
                nickname = StrictUtf8.GetString(frame, 43, nicknameLength);
            }
            catch (DecoderFallbackException error)
            {
                throw new SessionProtocolException("Welcome nickname is not UTF-8", error);
            }
            try
            {
                return new WelcomeSession(
                    requestId, sessionId, generation, serverTime, nickname);
            }
            catch (ArgumentException error)
            {
                throw new SessionProtocolException("Welcome fields are invalid", error);
            }
        }

        private static AuthenticationRejectedSession DecodeRejected(byte[] frame)
        {
            if (frame.Length != 19)
            {
                throw new SessionProtocolException("Rejected frame length is invalid");
            }
            ulong requestId = ReadUInt64(frame, 9);
            var reason = (AuthenticationRejectedReason)ReadUInt16(frame, 17);
            if (requestId == 0 || !IsValid(reason))
            {
                throw new SessionProtocolException("Rejected fields are invalid");
            }
            return new AuthenticationRejectedSession(requestId, reason);
        }

        private static SessionReplaced DecodeReplaced(byte[] frame)
        {
            if (frame.Length != 11 ||
                ReadUInt16(frame, 9) != (ushort)SessionReplacedReason.SameAccountLogin)
            {
                throw new SessionProtocolException("Replaced fields are invalid");
            }
            return new SessionReplaced(SessionReplacedReason.SameAccountLogin);
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

        private static byte[] CreateFrame(uint payloadLength, uint messageId)
        {
            var frame = new byte[payloadLength + 4];
            WriteUInt32(frame, 0, payloadLength);
            frame[4] = ProtocolMajor;
            WriteUInt32(frame, 5, messageId);
            return frame;
        }

        private static ushort ReadUInt16(byte[] bytes, int offset)
        {
            return (ushort)(((uint)bytes[offset] << 8) | bytes[offset + 1]);
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
            return ((ulong)ReadUInt32(bytes, offset) << 32) |
                   ReadUInt32(bytes, offset + 4);
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
