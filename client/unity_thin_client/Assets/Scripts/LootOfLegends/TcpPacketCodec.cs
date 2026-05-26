using System;

namespace LootOfLegends.ThinClient
{
    internal enum TcpPacketType : ushort
    {
        Welcome = 0x0001,
        ClientListSnapshot = 0x0002,
        CreateRoomRequest = 0x0101,
        CreateRoomResponse = 0x0102,
        JoinRoomRequest = 0x0103,
        JoinRoomResponse = 0x0104,
        LeaveRoomRequest = 0x0105,
        LeaveRoomResponse = 0x0106,
        RoomListSnapshot = 0x0107,
        ReadyRoomRequest = 0x0108,
        ReadyRoomResponse = 0x0109,
        BattleStart = 0x010A,
        MonsterSpawn = 0x010B,
        MonsterDeathRequest = 0x010C,
        MonsterDeath = 0x010D,
        DropListSnapshot = 0x010E,
        ClickLootRequest = 0x010F,
        LootResolved = 0x0110,
        LootRejected = 0x0111,
        InventorySnapshot = 0x0112,
        SmokeCreateCenterDropRequest = 0x0116,
        SmokePlacePlayersAroundCenterDropRequest = 0x0117,
        Error = 0x01FF
    }

    internal enum TcpErrorCode : ushort
    {
        None = 0,
        Full = 1,
        NotFound = 2,
        AlreadyInRoom = 3,
        NotInRoom = 4
    }

    internal enum TcpLootRejectReason : ushort
    {
        None = 0,
        AlreadyClaimed = 1,
        Overweight = 2
    }

    internal enum TcpDropSourceStatus
    {
        None,
        Observed,
        Ambiguous
    }

    internal struct TcpDropSourceObservation
    {
        public TcpDropSourceStatus Status;
        public uint RoomId;
        public uint DropId;
        public uint ItemId;
        public ushort Quantity;
        public ushort DropCount;
    }

    internal struct TcpLootResolvedObservation
    {
        public bool HasValue;
        public uint RoomId;
        public uint DropId;
        public ulong WinnerSessionId;
        public uint ItemId;
        public ushort Quantity;
    }

    internal struct TcpLootRejectedObservation
    {
        public bool HasValue;
        public uint RoomId;
        public uint DropId;
        public TcpLootRejectReason Reason;
    }

    internal struct TcpInventoryEntryObservation
    {
        public uint ItemId;
        public ushort Quantity;
    }

    internal struct TcpInventorySnapshotObservation
    {
        public bool HasValue;
        public ulong SessionId;
        public ushort CurrentWeight;
        public ushort MaxWeight;
        public TcpInventoryEntryObservation[] Entries;

        public bool ContainsItem(uint itemId, ushort quantity)
        {
            if (Entries == null)
            {
                return false;
            }

            for (int i = 0; i < Entries.Length; ++i)
            {
                if (Entries[i].ItemId == itemId && Entries[i].Quantity >= quantity)
                {
                    return true;
                }
            }

            return false;
        }
    }

    internal sealed class TcpDebugClientState
    {
        public ulong SessionId;
        public bool HasSessionId;
        public uint RoomId;
        public bool HasRoomId;
        public ushort PlayerCount;
        public ushort ReadyPlayerCount;
        public ushort TotalPlayerCount;
        public bool BattleStarted;
        public uint MonsterId;
        public bool HasMonsterId;
        public TcpDropSourceObservation DropSource;
        public TcpLootResolvedObservation LatestLootResolved;
        public TcpLootRejectedObservation LatestLootRejected;
        public TcpInventorySnapshotObservation LatestInventorySnapshot;

        public void Reset()
        {
            SessionId = 0;
            HasSessionId = false;
            ResetRoomRuntimeState();
        }

        public void ResetRoomRuntimeState()
        {
            RoomId = 0;
            HasRoomId = false;
            PlayerCount = 0;
            ReadyPlayerCount = 0;
            TotalPlayerCount = 0;
            BattleStarted = false;
            MonsterId = 0;
            HasMonsterId = false;
            DropSource = new TcpDropSourceObservation
            {
                Status = TcpDropSourceStatus.None
            };
            LatestLootResolved = new TcpLootResolvedObservation();
            LatestLootRejected = new TcpLootRejectedObservation();
            LatestInventorySnapshot = new TcpInventorySnapshotObservation
            {
                Entries = new TcpInventoryEntryObservation[0]
            };
        }
    }

    internal static class TcpPacketCodec
    {
        public const int HeaderSize = 4;
        public const int MaxPacketSize = 1024;

        private const int WelcomePacketSize = 12;
        private const int RoomStatusPacketSize = 10;
        private const int RoomIdPacketSize = 8;
        private const int ReadyRoomStatusPacketSize = 12;
        private const int BattleStartPacketSize = 24;
        private const int MonsterSpawnPacketSize = 18;
        private const int DropListSnapshotFixedPacketSize = HeaderSize + 4 + 2;
        private const int DropEntrySize = 4 + 4 + 2;
        private const int ClickLootRequestPacketSize = HeaderSize + 4;
        private const int LootResolvedPacketSize = HeaderSize + 4 + 4 + 8 + 4 + 2;
        private const int LootRejectedPacketSize = HeaderSize + 4 + 4 + 2;
        private const int InventorySnapshotFixedPacketSize = HeaderSize + 8 + 2 + 2 + 2;
        private const int InventoryEntrySize = 4 + 2;
        private const int ErrorPacketSize = 8;

        public static byte[] SerializeCreateRoomRequest()
        {
            return SerializeHeaderOnly(TcpPacketType.CreateRoomRequest);
        }

        public static byte[] SerializeReadyRoomRequest()
        {
            return SerializeHeaderOnly(TcpPacketType.ReadyRoomRequest);
        }

        public static byte[] SerializeSmokeCreateCenterDropRequest()
        {
            return SerializeHeaderOnly(TcpPacketType.SmokeCreateCenterDropRequest);
        }

        public static byte[] SerializeSmokePlacePlayersAroundCenterDropRequest()
        {
            return SerializeHeaderOnly(TcpPacketType.SmokePlacePlayersAroundCenterDropRequest);
        }

        public static byte[] SerializeJoinRoomRequest(uint roomId)
        {
            byte[] packet = new byte[RoomIdPacketSize];
            WriteHeader(packet, TcpPacketType.JoinRoomRequest);
            WriteU32(packet, HeaderSize, roomId);
            return packet;
        }

        public static byte[] SerializeClickLootRequest(uint dropId)
        {
            byte[] packet = new byte[ClickLootRequestPacketSize];
            WriteHeader(packet, TcpPacketType.ClickLootRequest);
            WriteU32(packet, HeaderSize, dropId);
            return packet;
        }

        public static bool TryPeekHeader(
            byte[] data,
            int size,
            out int packetSize,
            out TcpPacketType packetType,
            out ushort rawType,
            out string error)
        {
            packetSize = 0;
            packetType = TcpPacketType.Welcome;
            rawType = 0;
            error = string.Empty;

            if (data == null || size < HeaderSize)
            {
                error = "packet header is incomplete";
                return false;
            }

            packetSize = ReadU16(data, 0);
            rawType = ReadU16(data, 2);
            packetType = (TcpPacketType)rawType;
            if (packetSize < HeaderSize || packetSize > MaxPacketSize)
            {
                error = "invalid packet size " + packetSize;
                return false;
            }

            return true;
        }

        public static bool TryDescribePacket(
            string alias,
            TcpDebugClientState state,
            byte[] packet,
            out string description,
            out string error)
        {
            description = string.Empty;
            error = string.Empty;

            if (!TryPeekHeader(packet, packet.Length, out int packetSize, out TcpPacketType packetType, out ushort rawType, out error))
            {
                return false;
            }
            if (packet.Length != packetSize)
            {
                error = alias + " received partial packet: expected " + packetSize + ", got " + packet.Length;
                return false;
            }

            switch (packetType)
            {
                case TcpPacketType.Welcome:
                    if (!ExpectSize(packet, WelcomePacketSize, "Welcome", out error))
                    {
                        return false;
                    }
                    state.SessionId = ReadU64(packet, HeaderSize);
                    state.HasSessionId = true;
                    description = alias + " Welcome(sessionId=" + state.SessionId + ")";
                    return true;

                case TcpPacketType.ClientListSnapshot:
                    return DescribeClientListSnapshot(alias, packet, out description, out error);

                case TcpPacketType.CreateRoomResponse:
                    if (!DescribeRoomStatus(packet, "CreateRoomResponse", out uint createdRoomId, out ushort createdPlayerCount, out error))
                    {
                        return false;
                    }
                    state.ResetRoomRuntimeState();
                    state.RoomId = createdRoomId;
                    state.HasRoomId = true;
                    state.PlayerCount = createdPlayerCount;
                    description = alias + " CreateRoomResponse(roomId=" + createdRoomId + ", players=" + createdPlayerCount + ")";
                    return true;

                case TcpPacketType.JoinRoomResponse:
                    if (!DescribeRoomStatus(packet, "JoinRoomResponse", out uint joinedRoomId, out ushort joinedPlayerCount, out error))
                    {
                        return false;
                    }
                    state.ResetRoomRuntimeState();
                    state.RoomId = joinedRoomId;
                    state.HasRoomId = true;
                    state.PlayerCount = joinedPlayerCount;
                    description = alias + " JoinRoomResponse(roomId=" + joinedRoomId + ", players=" + joinedPlayerCount + ")";
                    return true;

                case TcpPacketType.RoomListSnapshot:
                    return DescribeRoomListSnapshot(alias, packet, out description, out error);

                case TcpPacketType.ReadyRoomResponse:
                    if (!ExpectSize(packet, ReadyRoomStatusPacketSize, "ReadyRoomResponse", out error))
                    {
                        return false;
                    }
                    state.RoomId = ReadU32(packet, HeaderSize);
                    state.HasRoomId = true;
                    state.ReadyPlayerCount = ReadU16(packet, HeaderSize + 4);
                    state.TotalPlayerCount = ReadU16(packet, HeaderSize + 6);
                    description = alias + " ReadyRoomResponse(roomId=" + state.RoomId +
                                  ", ready=" + state.ReadyPlayerCount + "/" + state.TotalPlayerCount + ")";
                    return true;

                case TcpPacketType.BattleStart:
                    if (!ExpectSize(packet, BattleStartPacketSize, "BattleStart", out error))
                    {
                        return false;
                    }
                    state.RoomId = ReadU32(packet, HeaderSize);
                    state.HasRoomId = true;
                    state.BattleStarted = true;
                    ulong playerA = ReadU64(packet, HeaderSize + 4);
                    ulong playerB = ReadU64(packet, HeaderSize + 12);
                    description = alias + " BattleStart(roomId=" + state.RoomId + ", players=" + playerA + "/" + playerB + ")";
                    return true;

                case TcpPacketType.MonsterSpawn:
                    if (!ExpectSize(packet, MonsterSpawnPacketSize, "MonsterSpawn", out error))
                    {
                        return false;
                    }
                    state.RoomId = ReadU32(packet, HeaderSize);
                    state.HasRoomId = true;
                    state.MonsterId = ReadU32(packet, HeaderSize + 4);
                    state.HasMonsterId = true;
                    uint monsterTypeId = ReadU32(packet, HeaderSize + 8);
                    ushort maxHp = ReadU16(packet, HeaderSize + 12);
                    description = alias + " MonsterSpawn(roomId=" + state.RoomId +
                                  ", monsterId=" + state.MonsterId +
                                  ", typeId=" + monsterTypeId +
                                  ", maxHp=" + maxHp + ")";
                    return true;

                case TcpPacketType.DropListSnapshot:
                    return DescribeDropListSnapshot(alias, state, packet, out description, out error);

                case TcpPacketType.LootResolved:
                    return DescribeLootResolved(alias, state, packet, out description, out error);

                case TcpPacketType.LootRejected:
                    return DescribeLootRejected(alias, state, packet, out description, out error);

                case TcpPacketType.InventorySnapshot:
                    return DescribeInventorySnapshot(alias, state, packet, out description, out error);

                case TcpPacketType.Error:
                    if (!ExpectSize(packet, ErrorPacketSize, "Error", out error))
                    {
                        return false;
                    }
                    TcpPacketType failedType = (TcpPacketType)ReadU16(packet, HeaderSize);
                    TcpErrorCode errorCode = (TcpErrorCode)ReadU16(packet, HeaderSize + 2);
                    description = alias + " Error(failedType=" + PacketTypeName(failedType) +
                                  ", code=" + ErrorCodeName(errorCode) + ")";
                    return true;

                default:
                    description = alias + " Packet(type=0x" + rawType.ToString("X4") + ", size=" + packet.Length + ")";
                    return true;
            }
        }

        private static byte[] SerializeHeaderOnly(TcpPacketType type)
        {
            byte[] packet = new byte[HeaderSize];
            WriteHeader(packet, type);
            return packet;
        }

        private static void WriteHeader(byte[] packet, TcpPacketType type)
        {
            WriteU16(packet, 0, (ushort)packet.Length);
            WriteU16(packet, 2, (ushort)type);
        }

        private static bool DescribeRoomStatus(
            byte[] packet,
            string packetName,
            out uint roomId,
            out ushort playerCount,
            out string error)
        {
            roomId = 0;
            playerCount = 0;
            if (!ExpectSize(packet, RoomStatusPacketSize, packetName, out error))
            {
                return false;
            }

            roomId = ReadU32(packet, HeaderSize);
            playerCount = ReadU16(packet, HeaderSize + 4);
            return true;
        }

        private static bool DescribeClientListSnapshot(
            string alias,
            byte[] packet,
            out string description,
            out string error)
        {
            description = string.Empty;
            error = string.Empty;
            if (packet.Length < HeaderSize + 2)
            {
                error = "ClientListSnapshot is too short";
                return false;
            }

            ushort count = ReadU16(packet, HeaderSize);
            int expectedSize = HeaderSize + 2 + (count * 8);
            if (packet.Length != expectedSize)
            {
                error = "ClientListSnapshot size mismatch: expected " + expectedSize + ", got " + packet.Length;
                return false;
            }

            description = alias + " ClientListSnapshot(count=" + count + ")";
            return true;
        }

        private static bool DescribeRoomListSnapshot(
            string alias,
            byte[] packet,
            out string description,
            out string error)
        {
            description = string.Empty;
            error = string.Empty;
            if (packet.Length < HeaderSize + 2)
            {
                error = "RoomListSnapshot is too short";
                return false;
            }

            ushort count = ReadU16(packet, HeaderSize);
            int expectedSize = HeaderSize + 2 + (count * 8);
            if (packet.Length != expectedSize)
            {
                error = "RoomListSnapshot size mismatch: expected " + expectedSize + ", got " + packet.Length;
                return false;
            }

            description = alias + " RoomListSnapshot(count=" + count + ")";
            return true;
        }

        private static bool DescribeDropListSnapshot(
            string alias,
            TcpDebugClientState state,
            byte[] packet,
            out string description,
            out string error)
        {
            description = string.Empty;
            error = string.Empty;
            if (packet.Length < DropListSnapshotFixedPacketSize)
            {
                error = "DropListSnapshot is too short";
                return false;
            }

            uint roomId = ReadU32(packet, HeaderSize);
            ushort count = ReadU16(packet, HeaderSize + 4);
            int expectedSize = DropListSnapshotFixedPacketSize + (count * DropEntrySize);
            if (packet.Length != expectedSize)
            {
                error = "DropListSnapshot size mismatch: expected " + expectedSize + ", got " + packet.Length;
                return false;
            }

            if (count == 0)
            {
                state.DropSource = new TcpDropSourceObservation
                {
                    Status = TcpDropSourceStatus.None,
                    RoomId = roomId,
                    DropCount = count
                };
                description = alias + " DropListSnapshot(roomId=" + roomId +
                              ", drops=0, server-origin dropId=-)";
                return true;
            }

            if (count > 1)
            {
                state.DropSource = new TcpDropSourceObservation
                {
                    Status = TcpDropSourceStatus.Ambiguous,
                    RoomId = roomId,
                    DropCount = count
                };
                description = alias + " DropListSnapshot(roomId=" + roomId +
                              ", drops=" + count +
                              ", server-origin dropId=ambiguous)";
                return true;
            }

            int offset = DropListSnapshotFixedPacketSize;
            uint dropId = ReadU32(packet, offset);
            uint itemId = ReadU32(packet, offset + 4);
            ushort quantity = ReadU16(packet, offset + 8);
            state.DropSource = new TcpDropSourceObservation
            {
                Status = TcpDropSourceStatus.Observed,
                RoomId = roomId,
                DropId = dropId,
                ItemId = itemId,
                Quantity = quantity,
                DropCount = count
            };
            description = alias + " DropListSnapshot(roomId=" + roomId +
                          ", server-origin dropId=" + dropId +
                          ", itemId=" + itemId +
                          ", quantity=" + quantity + ")";
            return true;
        }

        private static bool DescribeLootResolved(
            string alias,
            TcpDebugClientState state,
            byte[] packet,
            out string description,
            out string error)
        {
            description = string.Empty;
            if (!ExpectSize(packet, LootResolvedPacketSize, "LootResolved", out error))
            {
                return false;
            }

            uint roomId = ReadU32(packet, HeaderSize);
            uint dropId = ReadU32(packet, HeaderSize + 4);
            ulong winnerSessionId = ReadU64(packet, HeaderSize + 8);
            uint itemId = ReadU32(packet, HeaderSize + 16);
            ushort quantity = ReadU16(packet, HeaderSize + 20);
            state.LatestLootResolved = new TcpLootResolvedObservation
            {
                HasValue = true,
                RoomId = roomId,
                DropId = dropId,
                WinnerSessionId = winnerSessionId,
                ItemId = itemId,
                Quantity = quantity
            };
            description = alias + " server-origin LootResolved(roomId=" + roomId +
                          ", dropId=" + dropId +
                          ", winner=" + winnerSessionId +
                          ", itemId=" + itemId +
                          ", quantity=" + quantity + ")";
            return true;
        }

        private static bool DescribeLootRejected(
            string alias,
            TcpDebugClientState state,
            byte[] packet,
            out string description,
            out string error)
        {
            description = string.Empty;
            if (!ExpectSize(packet, LootRejectedPacketSize, "LootRejected", out error))
            {
                return false;
            }

            uint roomId = ReadU32(packet, HeaderSize);
            uint dropId = ReadU32(packet, HeaderSize + 4);
            TcpLootRejectReason reason = (TcpLootRejectReason)ReadU16(packet, HeaderSize + 8);
            state.LatestLootRejected = new TcpLootRejectedObservation
            {
                HasValue = true,
                RoomId = roomId,
                DropId = dropId,
                Reason = reason
            };
            description = alias + " server-origin LootRejected(roomId=" + roomId +
                          ", dropId=" + dropId +
                          ", reason=" + LootRejectReasonName(reason) + ")";
            return true;
        }

        private static bool DescribeInventorySnapshot(
            string alias,
            TcpDebugClientState state,
            byte[] packet,
            out string description,
            out string error)
        {
            description = string.Empty;
            error = string.Empty;
            if (packet.Length < InventorySnapshotFixedPacketSize)
            {
                error = "InventorySnapshot is too short";
                return false;
            }

            ulong sessionId = ReadU64(packet, HeaderSize);
            ushort currentWeight = ReadU16(packet, HeaderSize + 8);
            ushort maxWeight = ReadU16(packet, HeaderSize + 10);
            ushort count = ReadU16(packet, HeaderSize + 12);
            int expectedSize = InventorySnapshotFixedPacketSize + (count * InventoryEntrySize);
            if (packet.Length != expectedSize)
            {
                error = "InventorySnapshot size mismatch: expected " + expectedSize + ", got " + packet.Length;
                return false;
            }

            TcpInventoryEntryObservation[] entries = new TcpInventoryEntryObservation[count];
            int offset = InventorySnapshotFixedPacketSize;
            for (int i = 0; i < count; ++i)
            {
                entries[i] = new TcpInventoryEntryObservation
                {
                    ItemId = ReadU32(packet, offset),
                    Quantity = ReadU16(packet, offset + 4)
                };
                offset += InventoryEntrySize;
            }

            state.LatestInventorySnapshot = new TcpInventorySnapshotObservation
            {
                HasValue = true,
                SessionId = sessionId,
                CurrentWeight = currentWeight,
                MaxWeight = maxWeight,
                Entries = entries
            };
            description = alias + " server-origin InventorySnapshot(sessionId=" + sessionId +
                          ", weight=" + currentWeight + "/" + maxWeight +
                          ", items=" + count + ")";
            return true;
        }

        private static bool ExpectSize(byte[] packet, int expectedSize, string packetName, out string error)
        {
            error = string.Empty;
            if (packet.Length == expectedSize)
            {
                return true;
            }

            error = packetName + " size mismatch: expected " + expectedSize + ", got " + packet.Length;
            return false;
        }

        private static string PacketTypeName(TcpPacketType type)
        {
            switch (type)
            {
                case TcpPacketType.Welcome:
                    return "Welcome";
                case TcpPacketType.CreateRoomRequest:
                    return "CreateRoomRequest";
                case TcpPacketType.JoinRoomRequest:
                    return "JoinRoomRequest";
                case TcpPacketType.ReadyRoomRequest:
                    return "ReadyRoomRequest";
                case TcpPacketType.DropListSnapshot:
                    return "DropListSnapshot";
                case TcpPacketType.ClickLootRequest:
                    return "ClickLootRequest";
                case TcpPacketType.LootResolved:
                    return "LootResolved";
                case TcpPacketType.LootRejected:
                    return "LootRejected";
                case TcpPacketType.InventorySnapshot:
                    return "InventorySnapshot";
                case TcpPacketType.SmokeCreateCenterDropRequest:
                    return "SmokeCreateCenterDropRequest";
                case TcpPacketType.SmokePlacePlayersAroundCenterDropRequest:
                    return "SmokePlacePlayersAroundCenterDropRequest";
                default:
                    return "0x" + ((ushort)type).ToString("X4");
            }
        }

        private static string LootRejectReasonName(TcpLootRejectReason reason)
        {
            switch (reason)
            {
                case TcpLootRejectReason.None:
                    return "None";
                case TcpLootRejectReason.AlreadyClaimed:
                    return "AlreadyClaimed";
                case TcpLootRejectReason.Overweight:
                    return "Overweight";
                default:
                    return "0x" + ((ushort)reason).ToString("X4");
            }
        }

        private static string ErrorCodeName(TcpErrorCode code)
        {
            switch (code)
            {
                case TcpErrorCode.None:
                    return "None";
                case TcpErrorCode.Full:
                    return "Full";
                case TcpErrorCode.NotFound:
                    return "NotFound";
                case TcpErrorCode.AlreadyInRoom:
                    return "AlreadyInRoom";
                case TcpErrorCode.NotInRoom:
                    return "NotInRoom";
                default:
                    return "0x" + ((ushort)code).ToString("X4");
            }
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

        private static ulong ReadU64(byte[] data, int offset)
        {
            ulong value = 0;
            for (int i = 0; i < 8; ++i)
            {
                value = (value << 8) | data[offset + i];
            }
            return value;
        }

        private static void WriteU16(byte[] data, int offset, ushort value)
        {
            data[offset] = (byte)((value >> 8) & 0xFF);
            data[offset + 1] = (byte)(value & 0xFF);
        }

        private static void WriteU32(byte[] data, int offset, uint value)
        {
            data[offset] = (byte)((value >> 24) & 0xFF);
            data[offset + 1] = (byte)((value >> 16) & 0xFF);
            data[offset + 2] = (byte)((value >> 8) & 0xFF);
            data[offset + 3] = (byte)(value & 0xFF);
        }
    }
}
