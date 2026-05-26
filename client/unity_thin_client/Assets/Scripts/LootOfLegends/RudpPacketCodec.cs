using System;
using System.Collections.Generic;

namespace LootOfLegends.ThinClient
{
    internal static class RudpPacketCodec
    {
        public const int HeaderSize = 28;
        public const int HelloPayloadSize = 14;
        public const int InputCommandReadyPayloadSize = 10;
        public const int InputCommandMovePayloadSize = 16;
        public const int BattleStartPayloadSize = 20;
        public const int StateSnapshotFixedPayloadSize = 12;
        public const int StateSnapshotPlayerEntrySize = 16;
        public const float StateSnapshotPositionScale = 1000f;

        private const byte Magic0 = 0x4C;
        private const byte Magic1 = 0x4F;
        private const byte Version = 0x01;
        private const byte ReliableFlag = 0x01;
        private const byte AckOnlyFlag = 0x02;
        private const byte ControlChannel = 0x01;
        private const byte InputChannel = 0x02;
        private const byte SnapshotChannel = 0x03;
        private const byte EventChannel = 0x04;
        private const byte ReadyInputCommandOp = 0x01;
        private const byte MoveInputCommandOp = 0x04;
        private const byte StateSnapshotVersion = 0x01;
        private const byte StateSnapshotKindRoomMovementPlayers = 0x01;
        private const ushort LootResolvedGameEventType = 0x0002;
        private const ushort HelloPacketType = 0x1001;
        private const ushort InputCommandPacketType = 0x1002;
        private const ushort BattleStartPacketType = 0x1003;
        private const ushort StateSnapshotPacketType = 0x1004;
        private const ushort GameEventPacketType = 0x1005;
        private const ushort MetaResponsePacketType = 0x1006;
        private const ushort ErrorPacketType = 0x2001;
        private const byte AllowedFlags = (byte)(ReliableFlag | AckOnlyFlag);
        private const int GameEventFrameHeaderSize = 4;
        private const int LootResolvedGameEventBodySize = 22;
        private const int LootResolvedGameEventPayloadSize = GameEventFrameHeaderSize + LootResolvedGameEventBodySize;

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

        public struct RudpPacketHeader
        {
            public byte Flags;
            public byte ChannelId;
            public ushort PacketType;
            public uint Sequence;
            public uint Ack;
            public uint AckBits;
            public ushort PayloadLen;
        }

        public struct RudpBattleStartPayload
        {
            public uint RoomId;
            public ulong PlayerASessionId;
            public ulong PlayerBSessionId;
        }

        public struct RudpLootResolvedGameEventPayload
        {
            public uint RoomId;
            public uint DropId;
            public ulong WinnerSessionId;
            public uint ItemId;
            public ushort Quantity;
        }

        public struct RudpStateSnapshotPlayer
        {
            public ulong SessionId;
            public int PosX;
            public int PosY;

            public float WorldX
            {
                get { return PosX / StateSnapshotPositionScale; }
            }

            public float WorldY
            {
                get { return PosY / StateSnapshotPositionScale; }
            }
        }

        public struct RudpStateSnapshotPayload
        {
            public uint RoomId;
            public uint ServerTick;
            public RudpStateSnapshotPlayer[] Players;
        }

        public static byte[] SerializeHello(uint sequence, ushort clientVersion, uint clientId, ulong sessionId)
        {
            byte[] payload = new byte[HelloPayloadSize];
            WriteU16(payload, 0, clientVersion);
            WriteU32(payload, 2, clientId);
            WriteU64(payload, 6, sessionId);

            return SerializePacket(ReliableFlag, ControlChannel, HelloPacketType, sequence, payload);
        }

        public static byte[] SerializeAckOnly(uint sequence, uint ack, uint ackBits)
        {
            return SerializePacket(
                AckOnlyFlag,
                ControlChannel,
                HelloPacketType,
                sequence,
                ack,
                ackBits,
                new byte[0]);
        }

        public static byte[] SerializeReadyInputCommand(uint sequence, uint playerId, uint cmdSeq)
        {
            byte[] payload = new byte[InputCommandReadyPayloadSize];
            WriteU32(payload, 0, playerId);
            WriteU32(payload, 4, cmdSeq);
            payload[8] = ReadyInputCommandOp;
            payload[9] = 0;

            return SerializePacket(0, InputChannel, InputCommandPacketType, sequence, payload);
        }

        public static byte[] SerializeMoveInputCommand(
            uint sequence,
            uint playerId,
            uint cmdSeq,
            short dirX,
            short dirY)
        {
            byte[] payload = new byte[InputCommandMovePayloadSize];
            WriteU32(payload, 0, playerId);
            WriteU32(payload, 4, cmdSeq);
            payload[8] = MoveInputCommandOp;
            payload[9] = 6;
            WriteI16(payload, 10, dirX);
            WriteI16(payload, 12, dirY);
            WriteU16(payload, 14, 0);

            return SerializePacket(0, InputChannel, InputCommandPacketType, sequence, payload);
        }

        public static bool TryParsePacket(
            byte[] data,
            int size,
            out RudpPacketHeader header,
            out byte[] payload,
            out string error)
        {
            header = new RudpPacketHeader();
            payload = new byte[0];

            if (data == null || size < HeaderSize || size > data.Length)
            {
                error = "invalid packet size";
                return false;
            }
            if (data[0] != Magic0 || data[1] != Magic1)
            {
                error = "invalid magic";
                return false;
            }
            if (data[VersionOffset] != Version)
            {
                error = "invalid version";
                return false;
            }
            if (data[HeaderLenOffset] != HeaderSize)
            {
                error = "invalid header length";
                return false;
            }
            if (ReadU16(data, ReservedOffset) != 0)
            {
                error = "invalid reserved field";
                return false;
            }

            ushort payloadLen = ReadU16(data, PayloadLenOffset);
            if (payloadLen != size - HeaderSize)
            {
                error = "payload length mismatch";
                return false;
            }

            uint expectedChecksum = ReadU32(data, ChecksumOffset);
            byte[] checksumInput = new byte[size];
            Buffer.BlockCopy(data, 0, checksumInput, 0, size);
            WriteU32(checksumInput, ChecksumOffset, 0);
            uint actualChecksum = ComputeCrc32(checksumInput, size);
            if (actualChecksum != expectedChecksum)
            {
                error = "checksum mismatch";
                return false;
            }

            RudpPacketHeader parsed = new RudpPacketHeader
            {
                Flags = data[FlagsOffset],
                ChannelId = data[ChannelIdOffset],
                PacketType = ReadU16(data, PacketTypeOffset),
                Sequence = ReadU32(data, SequenceOffset),
                Ack = ReadU32(data, AckOffset),
                AckBits = ReadU32(data, AckBitsOffset),
                PayloadLen = payloadLen
            };
            if (!ValidateSemantics(parsed, out error))
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
            error = string.Empty;
            return true;
        }

        public static bool TryParseBattleStartPayload(
            byte[] data,
            int size,
            out RudpBattleStartPayload payload,
            out string error)
        {
            payload = new RudpBattleStartPayload();
            if (data == null || size != BattleStartPayloadSize)
            {
                error = "invalid BattleStart payload size";
                return false;
            }

            RudpBattleStartPayload parsed = new RudpBattleStartPayload
            {
                RoomId = ReadU32(data, 0),
                PlayerASessionId = ReadU64(data, 4),
                PlayerBSessionId = ReadU64(data, 12)
            };
            if (parsed.RoomId == 0 ||
                parsed.PlayerASessionId == 0 ||
                parsed.PlayerBSessionId == 0 ||
                parsed.PlayerASessionId == parsed.PlayerBSessionId)
            {
                error = "invalid BattleStart payload fields";
                return false;
            }

            payload = parsed;
            error = string.Empty;
            return true;
        }

        public static bool TryParseLootResolvedGameEventPayload(
            byte[] data,
            int size,
            out RudpLootResolvedGameEventPayload payload,
            out string error)
        {
            payload = new RudpLootResolvedGameEventPayload();
            if (data == null || size != LootResolvedGameEventPayloadSize)
            {
                error = "invalid GameEvent.LootResolved payload size";
                return false;
            }
            if (ReadU16(data, 0) != LootResolvedGameEventType)
            {
                error = "unsupported GameEvent type";
                return false;
            }
            if (ReadU16(data, 2) != LootResolvedGameEventBodySize)
            {
                error = "GameEvent.LootResolved body length mismatch";
                return false;
            }

            RudpLootResolvedGameEventPayload parsed = new RudpLootResolvedGameEventPayload
            {
                RoomId = ReadU32(data, 4),
                DropId = ReadU32(data, 8),
                WinnerSessionId = ReadU64(data, 12),
                ItemId = ReadU32(data, 20),
                Quantity = ReadU16(data, 24)
            };
            if (parsed.RoomId == 0 || parsed.DropId == 0)
            {
                error = "invalid GameEvent.LootResolved identity fields";
                return false;
            }

            payload = parsed;
            error = string.Empty;
            return true;
        }

        public static bool TryParseStateSnapshotPayload(
            byte[] data,
            int size,
            out RudpStateSnapshotPayload payload,
            out string error)
        {
            payload = new RudpStateSnapshotPayload
            {
                Players = new RudpStateSnapshotPlayer[0]
            };
            if (data == null || size < StateSnapshotFixedPayloadSize || size > data.Length)
            {
                error = "invalid StateSnapshot payload size";
                return false;
            }
            if (data[0] != StateSnapshotVersion)
            {
                error = "unsupported StateSnapshot version";
                return false;
            }
            if (data[1] != StateSnapshotKindRoomMovementPlayers)
            {
                error = "unsupported StateSnapshot kind";
                return false;
            }

            ushort playerCount = ReadU16(data, 10);
            int expectedSize = StateSnapshotFixedPayloadSize + (playerCount * StateSnapshotPlayerEntrySize);
            if (size != expectedSize)
            {
                error = "StateSnapshot payload length mismatch";
                return false;
            }

            uint roomId = ReadU32(data, 2);
            if (roomId == 0)
            {
                error = "invalid StateSnapshot roomId";
                return false;
            }

            RudpStateSnapshotPlayer[] players = new RudpStateSnapshotPlayer[playerCount];
            HashSet<ulong> seenSessionIds = new HashSet<ulong>();
            int offset = StateSnapshotFixedPayloadSize;
            for (int i = 0; i < playerCount; ++i)
            {
                ulong sessionId = ReadU64(data, offset);
                if (sessionId == 0)
                {
                    error = "invalid StateSnapshot sessionId";
                    return false;
                }
                if (!seenSessionIds.Add(sessionId))
                {
                    error = "duplicate StateSnapshot sessionId";
                    return false;
                }

                players[i] = new RudpStateSnapshotPlayer
                {
                    SessionId = sessionId,
                    PosX = ReadI32(data, offset + 8),
                    PosY = ReadI32(data, offset + 12)
                };
                offset += StateSnapshotPlayerEntrySize;
            }

            payload = new RudpStateSnapshotPayload
            {
                RoomId = roomId,
                ServerTick = ReadU32(data, 6),
                Players = players
            };
            error = string.Empty;
            return true;
        }

        public static bool IsReliable(RudpPacketHeader header)
        {
            return (header.Flags & ReliableFlag) != 0;
        }

        public static bool IsBattleStart(RudpPacketHeader header)
        {
            return header.PacketType == BattleStartPacketType;
        }

        public static bool IsStateSnapshot(RudpPacketHeader header)
        {
            return header.PacketType == StateSnapshotPacketType;
        }

        public static bool IsGameEvent(RudpPacketHeader header)
        {
            return header.PacketType == GameEventPacketType;
        }

        private static byte[] SerializePacket(
            byte flags,
            byte channelId,
            ushort packetType,
            uint sequence,
            byte[] payload)
        {
            return SerializePacket(flags, channelId, packetType, sequence, 0, 0, payload);
        }

        private static byte[] SerializePacket(
            byte flags,
            byte channelId,
            ushort packetType,
            uint sequence,
            uint ack,
            uint ackBits,
            byte[] payload)
        {
            byte[] packet = new byte[HeaderSize + payload.Length];
            packet[0] = Magic0;
            packet[1] = Magic1;
            packet[VersionOffset] = Version;
            packet[FlagsOffset] = flags;
            packet[HeaderLenOffset] = HeaderSize;
            packet[ChannelIdOffset] = channelId;
            WriteU16(packet, PacketTypeOffset, packetType);
            WriteU32(packet, SequenceOffset, sequence);
            WriteU32(packet, AckOffset, ack);
            WriteU32(packet, AckBitsOffset, ackBits);
            WriteU16(packet, PayloadLenOffset, (ushort)payload.Length);
            WriteU16(packet, ReservedOffset, 0);
            Buffer.BlockCopy(payload, 0, packet, HeaderSize, payload.Length);

            uint checksum = ComputeCrc32(packet, packet.Length);
            WriteU32(packet, ChecksumOffset, checksum);
            return packet;
        }

        private static bool ValidateSemantics(RudpPacketHeader header, out string error)
        {
            if ((header.Flags & ~AllowedFlags) != 0)
            {
                error = "unsupported flags";
                return false;
            }
            if ((header.Flags & AckOnlyFlag) != 0 && header.PayloadLen != 0)
            {
                error = "AckOnly packet has payload";
                return false;
            }

            byte expectedChannel = ExpectedChannelForPacketType(header.PacketType);
            if (expectedChannel == 0)
            {
                error = "unsupported packet type";
                return false;
            }
            if (header.ChannelId != expectedChannel)
            {
                error = "packet channel mismatch";
                return false;
            }

            error = string.Empty;
            return true;
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
            for (int i = 0; i < size; ++i)
            {
                crc ^= data[i];
                for (int bit = 0; bit < 8; ++bit)
                {
                    if ((crc & 1U) != 0U)
                    {
                        crc = (crc >> 1) ^ Crc32Polynomial;
                    }
                    else
                    {
                        crc >>= 1;
                    }
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

        private static ulong ReadU64(byte[] data, int offset)
        {
            ulong value = 0;
            for (int i = 0; i < 8; ++i)
            {
                value = (value << 8) | data[offset + i];
            }
            return value;
        }

        private static void WriteU64(byte[] data, int offset, ulong value)
        {
            for (int i = 7; i >= 0; --i)
            {
                data[offset + 7 - i] = (byte)((value >> (i * 8)) & 0xFF);
            }
        }
    }
}
