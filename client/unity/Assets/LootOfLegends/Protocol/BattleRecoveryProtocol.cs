using System;

namespace LootOfLegends.Protocol
{
    public enum BattleRecoveryReason : byte
    {
        ResultGenerationFailed = 1,
        SettlementRecoveryPending = 2
    }

    public sealed class BattleRecoveryNotice
    {
        public BattleRecoveryNotice(
            ulong roomId,
            ulong battleInstanceId,
            BattleRecoveryReason reason)
        {
            if (roomId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(roomId));
            }
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            if (reason != BattleRecoveryReason.ResultGenerationFailed &&
                reason != BattleRecoveryReason.SettlementRecoveryPending)
            {
                throw new ArgumentOutOfRangeException(nameof(reason));
            }
            RoomId = roomId;
            BattleInstanceId = battleInstanceId;
            Reason = reason;
        }

        public ulong RoomId { get; }
        public ulong BattleInstanceId { get; }
        public BattleRecoveryReason Reason { get; }
    }

    public sealed class BattleRecoveryProtocolException : Exception
    {
        public BattleRecoveryProtocolException(string message)
            : base(message)
        {
        }
    }

    public static class BattleRecoveryProtocolCodec
    {
        private const int FrameBytes = 26;
        private const uint PayloadBytes = 22;
        private const byte ProtocolMajor = 1;
        private const uint BattleRecoveryNoticeMessageId = 37;

        public static BattleRecoveryNotice DecodeServerFrame(byte[] frame)
        {
            if (frame == null || frame.Length != FrameBytes)
            {
                throw new BattleRecoveryProtocolException(
                    "Battle recovery frame length is invalid");
            }
            if (ReadUInt32(frame, 0) != PayloadBytes ||
                frame[4] != ProtocolMajor ||
                ReadUInt32(frame, 5) != BattleRecoveryNoticeMessageId)
            {
                throw new BattleRecoveryProtocolException(
                    "Unsupported Battle recovery envelope");
            }
            ulong roomId = ReadUInt64(frame, 9);
            ulong battleId = ReadUInt64(frame, 17);
            byte rawReason = frame[25];
            if (roomId == 0 || battleId == 0 || rawReason < 1 || rawReason > 2)
            {
                throw new BattleRecoveryProtocolException(
                    "Malformed Battle recovery payload");
            }
            return new BattleRecoveryNotice(
                roomId, battleId, (BattleRecoveryReason)rawReason);
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
    }
}
