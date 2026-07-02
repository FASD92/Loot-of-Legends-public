using System;
using System.Collections.Generic;

namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerRudpPacketHeader
    {
        public readonly byte Flags;
        public readonly byte ChannelId;
        public readonly ushort PacketType;
        public readonly uint Sequence;
        public readonly uint Ack;
        public readonly uint AckBits;
        public readonly ushort PayloadLen;

        public PlayerRudpPacketHeader(
            byte flags,
            byte channelId,
            ushort packetType,
            uint sequence,
            uint ack,
            uint ackBits,
            ushort payloadLen)
        {
            Flags = flags;
            ChannelId = channelId;
            PacketType = packetType;
            Sequence = sequence;
            Ack = ack;
            AckBits = ackBits;
            PayloadLen = payloadLen;
        }
    }

    public static class PlayerRudpPacket
    {
        public const int HeaderSize = 28;
        public const int HelloPayloadSize = 14;
        public const int MoveInputCommandPayloadSize = 16;
        public const int AttackInputCommandPayloadSize = 14;
        public const int SpaceLootInputCommandPayloadSize = 10;
        public const int StateSnapshotFixedPayloadSize = 12;
        public const int StateSnapshotPlayerEntrySize = 16;

        private const byte Magic0 = 0x4C;
        private const byte Magic1 = 0x4F;
        private const byte Version = 0x01;
        private const byte ReliableFlag = 0x01;
        private const byte AckOnlyFlag = 0x02;
        private const byte UnreliableFlags = 0x00;
        private const byte ControlChannel = 0x01;
        private const byte InputChannel = 0x02;
        private const byte SnapshotChannel = 0x03;
        private const byte EventChannel = 0x04;
        private const ushort HelloPacketType = 0x1001;
        private const ushort InputCommandPacketType = 0x1002;
        private const ushort BattleStartPacketType = 0x1003;
        private const ushort StateSnapshotPacketType = 0x1004;
        private const ushort GameEventPacketType = 0x1005;
        private const ushort MetaResponsePacketType = 0x1006;
        private const ushort ErrorPacketType = 0x2001;
        private const byte MoveInputCommandOp = 0x04;
        private const byte MoveInputCommandArgLen = 0x06;
        private const byte AttackInputCommandOp = 0x05;
        private const byte AttackInputCommandArgLen = 0x04;
        private const byte SpaceLootInputCommandOp = 0x06;
        private const byte SpaceLootInputCommandArgLen = 0x00;
        private const byte StateSnapshotVersion = 0x01;
        private const byte StateSnapshotKindRoomMovementPlayers = 0x01;
        private const byte AllowedFlags = ReliableFlag | AckOnlyFlag;

        private const int VersionOffset = 2;
        private const int FlagsOffset = 3;
        private const int HeaderLenOffset = 4;
        private const int ChannelIdOffset = 5;
        private const int PacketTypeOffset = 6;
        private const int SequenceOffset = 8;
        private const int AckOffset = 12;
        private const int AckBitsOffset = 16;
        private const int PayloadLenOffset = 20;
        private const int ChecksumOffset = 22;
        private const int ReservedOffset = 26;

        private const uint Crc32Polynomial = 0xEDB88320U;
        private const uint Crc32Initial = 0xFFFFFFFFU;
        private const uint Crc32FinalXor = 0xFFFFFFFFU;

        public static byte[] SerializeHello(uint sequence, PlayerRudpHello hello)
        {
            if (!hello.IsValid)
            {
                throw new ArgumentException("invalid RUDP Hello", nameof(hello));
            }

            byte[] packet = new byte[HeaderSize + HelloPayloadSize];
            packet[0] = Magic0;
            packet[1] = Magic1;
            packet[VersionOffset] = Version;
            packet[FlagsOffset] = ReliableFlag;
            packet[HeaderLenOffset] = HeaderSize;
            packet[ChannelIdOffset] = ControlChannel;
            WriteU16(packet, PacketTypeOffset, HelloPacketType);
            WriteU32(packet, SequenceOffset, sequence);
            WriteU32(packet, AckOffset, 0U);
            WriteU32(packet, AckBitsOffset, 0U);
            WriteU16(packet, PayloadLenOffset, HelloPayloadSize);
            WriteU16(packet, ReservedOffset, 0);

            WriteU16(packet, HeaderSize, hello.ClientVersion);
            WriteU32(packet, HeaderSize + 2, hello.ClientId);
            WriteU64(packet, HeaderSize + 6, hello.SessionId);

            uint checksum = ComputeCrc32(packet, packet.Length);
            WriteU32(packet, ChecksumOffset, checksum);
            return packet;
        }

        public static byte[] SerializeMoveInputCommand(
            uint sequence,
            uint playerId,
            uint commandSequence,
            short dirX,
            short dirY)
        {
            byte[] packet = new byte[HeaderSize + MoveInputCommandPayloadSize];
            WriteHeader(
                packet,
                UnreliableFlags,
                InputChannel,
                InputCommandPacketType,
                sequence,
                MoveInputCommandPayloadSize);

            WriteU32(packet, HeaderSize, playerId);
            WriteU32(packet, HeaderSize + 4, commandSequence);
            packet[HeaderSize + 8] = MoveInputCommandOp;
            packet[HeaderSize + 9] = MoveInputCommandArgLen;
            WriteI16(packet, HeaderSize + 10, dirX);
            WriteI16(packet, HeaderSize + 12, dirY);
            WriteU16(packet, HeaderSize + 14, 0);

            uint checksum = ComputeCrc32(packet, packet.Length);
            WriteU32(packet, ChecksumOffset, checksum);
            return packet;
        }

        public static byte[] SerializeAttackInputCommand(
            uint sequence,
            uint playerId,
            uint commandSequence,
            uint targetHintMonsterId)
        {
            byte[] packet = new byte[HeaderSize + AttackInputCommandPayloadSize];
            WriteHeader(
                packet,
                UnreliableFlags,
                InputChannel,
                InputCommandPacketType,
                sequence,
                AttackInputCommandPayloadSize);

            WriteU32(packet, HeaderSize, playerId);
            WriteU32(packet, HeaderSize + 4, commandSequence);
            packet[HeaderSize + 8] = AttackInputCommandOp;
            packet[HeaderSize + 9] = AttackInputCommandArgLen;
            WriteU32(packet, HeaderSize + 10, targetHintMonsterId);

            uint checksum = ComputeCrc32(packet, packet.Length);
            WriteU32(packet, ChecksumOffset, checksum);
            return packet;
        }

        public static byte[] SerializeSpaceLootInputCommand(
            uint sequence,
            uint playerId,
            uint commandSequence)
        {
            byte[] packet = new byte[HeaderSize + SpaceLootInputCommandPayloadSize];
            WriteHeader(
                packet,
                UnreliableFlags,
                InputChannel,
                InputCommandPacketType,
                sequence,
                SpaceLootInputCommandPayloadSize);

            WriteU32(packet, HeaderSize, playerId);
            WriteU32(packet, HeaderSize + 4, commandSequence);
            packet[HeaderSize + 8] = SpaceLootInputCommandOp;
            packet[HeaderSize + 9] = SpaceLootInputCommandArgLen;

            uint checksum = ComputeCrc32(packet, packet.Length);
            WriteU32(packet, ChecksumOffset, checksum);
            return packet;
        }

        public static bool TryParsePacket(
            byte[] data,
            int size,
            out PlayerRudpPacketHeader header,
            out byte[] payload)
        {
            header = default;
            payload = new byte[0];

            if (data == null ||
                size < HeaderSize ||
                size > data.Length ||
                data[0] != Magic0 ||
                data[1] != Magic1 ||
                data[VersionOffset] != Version ||
                data[HeaderLenOffset] != HeaderSize ||
                ReadU16(data, ReservedOffset) != 0)
            {
                return false;
            }

            ushort payloadLen = ReadU16(data, PayloadLenOffset);
            if (payloadLen != size - HeaderSize)
            {
                return false;
            }

            uint expectedChecksum = ReadU32(data, ChecksumOffset);
            byte[] checksumInput = new byte[size];
            Buffer.BlockCopy(data, 0, checksumInput, 0, size);
            WriteU32(checksumInput, ChecksumOffset, 0U);
            if (ComputeCrc32(checksumInput, size) != expectedChecksum)
            {
                return false;
            }

            PlayerRudpPacketHeader parsed = new PlayerRudpPacketHeader(
                data[FlagsOffset],
                data[ChannelIdOffset],
                ReadU16(data, PacketTypeOffset),
                ReadU32(data, SequenceOffset),
                ReadU32(data, AckOffset),
                ReadU32(data, AckBitsOffset),
                payloadLen);
            if (!ValidateSemantics(parsed))
            {
                return false;
            }

            byte[] parsedPayload = new byte[payloadLen];
            if (payloadLen > 0)
            {
                Buffer.BlockCopy(data, HeaderSize, parsedPayload, 0, payloadLen);
            }

            header = parsed;
            payload = parsedPayload;
            return true;
        }

        public static bool TryParseStateSnapshotPayload(
            byte[] data,
            int size,
            out PlayerRudpStateSnapshot snapshot)
        {
            snapshot = default;
            if (data == null ||
                size < StateSnapshotFixedPayloadSize ||
                size > data.Length ||
                data[0] != StateSnapshotVersion ||
                data[1] != StateSnapshotKindRoomMovementPlayers)
            {
                return false;
            }

            ushort playerCount = ReadU16(data, 10);
            int expectedSize =
                StateSnapshotFixedPayloadSize +
                (playerCount * StateSnapshotPlayerEntrySize);
            if (size != expectedSize)
            {
                return false;
            }

            uint roomId = ReadU32(data, 2);
            if (roomId == 0U)
            {
                return false;
            }

            PlayerRudpStateSnapshotPlayer[] players =
                new PlayerRudpStateSnapshotPlayer[playerCount];
            HashSet<ulong> seenSessionIds = new HashSet<ulong>();
            int offset = StateSnapshotFixedPayloadSize;
            for (int index = 0; index < playerCount; ++index)
            {
                ulong sessionId = ReadU64(data, offset);
                if (sessionId == 0UL || !seenSessionIds.Add(sessionId))
                {
                    return false;
                }

                players[index] = new PlayerRudpStateSnapshotPlayer(
                    sessionId,
                    ReadI32(data, offset + 8),
                    ReadI32(data, offset + 12));
                offset += StateSnapshotPlayerEntrySize;
            }

            snapshot = new PlayerRudpStateSnapshot(roomId, ReadU32(data, 6), players);
            return true;
        }

        public static bool IsStateSnapshot(PlayerRudpPacketHeader header)
        {
            return header.PacketType == StateSnapshotPacketType;
        }

        private static void WriteHeader(
            byte[] packet,
            byte flags,
            byte channelId,
            ushort packetType,
            uint sequence,
            int payloadSize)
        {
            packet[0] = Magic0;
            packet[1] = Magic1;
            packet[VersionOffset] = Version;
            packet[FlagsOffset] = flags;
            packet[HeaderLenOffset] = HeaderSize;
            packet[ChannelIdOffset] = channelId;
            WriteU16(packet, PacketTypeOffset, packetType);
            WriteU32(packet, SequenceOffset, sequence);
            WriteU32(packet, AckOffset, 0U);
            WriteU32(packet, AckBitsOffset, 0U);
            WriteU16(packet, PayloadLenOffset, (ushort)payloadSize);
            WriteU16(packet, ReservedOffset, 0);
        }

        private static bool ValidateSemantics(PlayerRudpPacketHeader header)
        {
            if ((header.Flags & ~AllowedFlags) != 0)
            {
                return false;
            }
            if ((header.Flags & AckOnlyFlag) != 0 && header.PayloadLen != 0)
            {
                return false;
            }

            byte expectedChannel = ExpectedChannelForPacketType(header.PacketType);
            return expectedChannel != 0 && header.ChannelId == expectedChannel;
        }

        private static byte ExpectedChannelForPacketType(ushort packetType)
        {
            switch (packetType)
            {
                case HelloPacketType:
                case MetaResponsePacketType:
                case ErrorPacketType:
                    return ControlChannel;
                case InputCommandPacketType:
                    return InputChannel;
                case StateSnapshotPacketType:
                    return SnapshotChannel;
                case BattleStartPacketType:
                case GameEventPacketType:
                    return EventChannel;
                default:
                    return 0;
            }
        }

        private static uint ComputeCrc32(byte[] data, int size)
        {
            uint crc = Crc32Initial;
            for (int index = 0; index < size; ++index)
            {
                crc ^= data[index];
                for (int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc & 1U) != 0U
                        ? (crc >> 1) ^ Crc32Polynomial
                        : crc >> 1;
                }
            }

            return crc ^ Crc32FinalXor;
        }

        private static void WriteU16(byte[] data, int offset, ushort value)
        {
            data[offset] = (byte)((value >> 8) & 0xFF);
            data[offset + 1] = (byte)(value & 0xFF);
        }

        private static void WriteI16(byte[] data, int offset, short value)
        {
            WriteU16(data, offset, unchecked((ushort)value));
        }

        private static void WriteU32(byte[] data, int offset, uint value)
        {
            data[offset] = (byte)((value >> 24) & 0xFF);
            data[offset + 1] = (byte)((value >> 16) & 0xFF);
            data[offset + 2] = (byte)((value >> 8) & 0xFF);
            data[offset + 3] = (byte)(value & 0xFF);
        }

        private static ushort ReadU16(byte[] data, int offset)
        {
            return (ushort)((data[offset] << 8) | data[offset + 1]);
        }

        private static uint ReadU32(byte[] data, int offset)
        {
            return ((uint)data[offset] << 24) |
                ((uint)data[offset + 1] << 16) |
                ((uint)data[offset + 2] << 8) |
                data[offset + 3];
        }

        private static int ReadI32(byte[] data, int offset)
        {
            return unchecked((int)ReadU32(data, offset));
        }

        private static void WriteU64(byte[] data, int offset, ulong value)
        {
            for (int index = 7; index >= 0; --index)
            {
                data[offset + 7 - index] = (byte)((value >> (index * 8)) & 0xFF);
            }
        }

        private static ulong ReadU64(byte[] data, int offset)
        {
            ulong value = 0UL;
            for (int index = 0; index < 8; ++index)
            {
                value = (value << 8) | data[offset + index];
            }

            return value;
        }
    }
}
