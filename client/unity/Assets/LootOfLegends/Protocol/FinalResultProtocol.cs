using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Text;

namespace LootOfLegends.Protocol
{
    public enum FinalResultOutcome : byte
    {
        MonsterDefeated = 1,
        CombatTimeout = 2,
        CancelledNoActiveParticipants = 3
    }

    public enum FinalResultExitStatus : byte
    {
        TerminalPresent = 1,
        TerminalExited = 2
    }

    public sealed class FinalResultEntry
    {
        public FinalResultEntry(
            ulong sessionId,
            string nickname,
            FinalResultExitStatus exitStatus,
            ulong finalAssetValue,
            uint? rank,
            bool isTop)
        {
            if (sessionId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(sessionId));
            }
            if (string.IsNullOrEmpty(nickname))
            {
                throw new ArgumentException("Final result nickname cannot be empty", nameof(nickname));
            }
            if (exitStatus != FinalResultExitStatus.TerminalPresent &&
                exitStatus != FinalResultExitStatus.TerminalExited)
            {
                throw new ArgumentOutOfRangeException(nameof(exitStatus));
            }

            SessionId = sessionId;
            Nickname = nickname;
            ExitStatus = exitStatus;
            FinalAssetValue = finalAssetValue;
            Rank = rank;
            IsTop = isTop;
        }

        public ulong SessionId { get; }
        public string Nickname { get; }
        public FinalResultExitStatus ExitStatus { get; }
        public ulong FinalAssetValue { get; }
        public uint? Rank { get; }
        public bool IsTop { get; }
    }

    public sealed class BattleFinalResult
    {
        private readonly IReadOnlyList<FinalResultEntry> entries;

        public BattleFinalResult(
            ulong roomId,
            ulong battleInstanceId,
            FinalResultOutcome outcome,
            IReadOnlyList<FinalResultEntry> entries)
        {
            if (roomId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(roomId));
            }
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            if (outcome != FinalResultOutcome.MonsterDefeated &&
                outcome != FinalResultOutcome.CombatTimeout &&
                outcome != FinalResultOutcome.CancelledNoActiveParticipants)
            {
                throw new ArgumentOutOfRangeException(nameof(outcome));
            }
            if (entries == null || entries.Count < 2 || entries.Count > 10)
            {
                throw new ArgumentException("Final result requires 2 to 10 entries", nameof(entries));
            }

            var copy = new List<FinalResultEntry>(entries.Count);
            var sessions = new HashSet<ulong>();
            foreach (FinalResultEntry entry in entries)
            {
                if (entry == null || !sessions.Add(entry.SessionId))
                {
                    throw new ArgumentException("Final result entries must be unique", nameof(entries));
                }
                if (outcome == FinalResultOutcome.MonsterDefeated)
                {
                    if (entry.Rank == null || entry.Rank == 0 ||
                        entry.IsTop != (entry.Rank == 1))
                    {
                        throw new ArgumentException("Winner result rank is malformed", nameof(entries));
                    }
                }
                else if (entry.Rank != null || entry.IsTop)
                {
                    throw new ArgumentException("Non-winner result cannot contain rank", nameof(entries));
                }
                copy.Add(entry);
            }

            RoomId = roomId;
            BattleInstanceId = battleInstanceId;
            Outcome = outcome;
            this.entries = new ReadOnlyCollection<FinalResultEntry>(copy);
        }

        public ulong RoomId { get; }
        public ulong BattleInstanceId { get; }
        public FinalResultOutcome Outcome { get; }
        public IReadOnlyList<FinalResultEntry> Entries => entries;
    }

    public sealed class FinalResultProtocolException : Exception
    {
        public FinalResultProtocolException(string message)
            : base(message)
        {
        }

        public FinalResultProtocolException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    public static class FinalResultProtocolCodec
    {
        public const int MaximumServerPayloadBytes = 655552;

        private const byte ProtocolMajor = 1;
        private const uint FinalResultMessageId = 36;
        private static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);

        public static BattleFinalResult DecodeServerFrame(byte[] frame)
        {
            if (frame == null || frame.Length < 28)
            {
                throw new FinalResultProtocolException("Final result frame is partial");
            }
            uint payloadLength = ReadUInt32(frame, 0);
            if (payloadLength != frame.Length - 4 || payloadLength > MaximumServerPayloadBytes)
            {
                throw new FinalResultProtocolException("Final result frame length is invalid");
            }
            if (frame[4] != ProtocolMajor || ReadUInt32(frame, 5) != FinalResultMessageId)
            {
                throw new FinalResultProtocolException("Unsupported final result envelope");
            }

            ulong roomId = ReadUInt64(frame, 9);
            ulong battleId = ReadUInt64(frame, 17);
            byte rawOutcome = frame[25];
            int count = ReadUInt16(frame, 26);
            if (roomId == 0 || battleId == 0 || rawOutcome < 1 || rawOutcome > 3 ||
                count < 2 || count > 10)
            {
                throw new FinalResultProtocolException("Malformed final result prefix");
            }

            var outcome = (FinalResultOutcome)rawOutcome;
            var entries = new List<FinalResultEntry>(count);
            int offset = 28;
            for (int index = 0; index < count; index++)
            {
                RequireAvailable(frame, offset, 10);
                ulong sessionId = ReadUInt64(frame, offset);
                int nicknameLength = ReadUInt16(frame, offset + 8);
                offset += 10;
                RequireAvailable(frame, offset, nicknameLength + 14);
                string nickname;
                try
                {
                    nickname = StrictUtf8.GetString(frame, offset, nicknameLength);
                }
                catch (DecoderFallbackException error)
                {
                    throw new FinalResultProtocolException("Final result nickname is not UTF-8", error);
                }
                offset += nicknameLength;
                byte rawExit = frame[offset++];
                ulong assetValue = ReadUInt64(frame, offset);
                offset += 8;
                uint rawRank = ReadUInt32(frame, offset);
                offset += 4;
                byte rawTop = frame[offset++];
                if (sessionId == 0 || nicknameLength == 0 || rawExit < 1 || rawExit > 2 ||
                    rawTop > 1)
                {
                    throw new FinalResultProtocolException("Malformed final result entry");
                }
                try
                {
                    entries.Add(new FinalResultEntry(
                        sessionId,
                        nickname,
                        (FinalResultExitStatus)rawExit,
                        assetValue,
                        rawRank == 0 ? (uint?)null : rawRank,
                        rawTop == 1));
                }
                catch (ArgumentException error)
                {
                    throw new FinalResultProtocolException("Malformed final result entry", error);
                }
            }
            if (offset != frame.Length)
            {
                throw new FinalResultProtocolException("Final result has trailing bytes");
            }
            try
            {
                return new BattleFinalResult(roomId, battleId, outcome, entries);
            }
            catch (ArgumentException error)
            {
                throw new FinalResultProtocolException("Malformed final result", error);
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

        private static void RequireAvailable(byte[] bytes, int offset, int count)
        {
            if (count < 0 || offset < 0 || offset > bytes.Length - count)
            {
                throw new FinalResultProtocolException("Final result payload is truncated");
            }
        }
    }
}
