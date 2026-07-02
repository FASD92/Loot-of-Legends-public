using System;
using System.Globalization;
using System.Text;

namespace LootOfLegends.PlayerClient
{
    public static class PlayerTcpPacket
    {
        private static readonly UTF8Encoding StrictUtf8 = new UTF8Encoding(false, true);

        public const int HeaderSize = 4;
        public const int MaxPacketSize = 1024;
        public const int WelcomePacketSize = 12;
        public const int GameSessionTokenMaxBytes = 512;
        public const int GameSessionTokenLengthFieldSize = 2;
        public const int RoomIdPacketSize = HeaderSize + 4;
        public const int RoomStatusPacketSize = HeaderSize + 4 + 2;
        public const int CreateRoomResponsePacketSize = RoomStatusPacketSize;
        public const int ReadyRoomResponsePacketSize = HeaderSize + 4 + 2 + 2;
        public const int BattleStartPacketSize = HeaderSize + 4 + 8 + 8;
        public const int BattleLoadEntryMinPacketSize = HeaderSize + 4 + 8 + 2;
        public const int BattleLoadEntryEntrySize = 8;
        public const int BattleLoadEntryMinPlayers = 2;
        public const int BattleLoadEntryMaxPlayers = 10;
        public const int BattleFinalRankingMinPacketSize = HeaderSize + 4 + 8 + 1;
        public const int BattleFinalRankingMinRows = 1;
        public const int BattleFinalRankingMaxRows = 10;
        public const int BattleFinalRankingFixedRowSize = 2 + 8 + 1 + 8;
        public const int ArenaLoadCompletePacketSize = HeaderSize + 4 + 8;
        public const int ArenaGameplayStartPacketSize = HeaderSize + 4 + 8;
        public const int MonsterSpawnPacketSize = HeaderSize + 4 + 4 + 4 + 2;
        public const int MonsterHealthSnapshotPacketSize = HeaderSize + 4 + 4 + 2 + 2;
        public const int MonsterDeathPacketSize = HeaderSize + 4 + 4;
        public const int DropListSnapshotV2FixedPacketSize = HeaderSize + 4 + 4 + 2;
        public const int DropEntryV2Size = 4 + 4 + 2 + 4 + 4;
        public const int LootResolvedPacketSize = HeaderSize + 4 + 4 + 8 + 4 + 2;
        public const int LootRejectedPacketSize = HeaderSize + 4 + 4 + 2;
        public const int InventorySnapshotMinPacketSize = HeaderSize + 8 + 2 + 2 + 2;
        public const int InventoryEntrySize = 4 + 2;
        public const int ClientListSnapshotMinPacketSize = HeaderSize + 2;
        public const int RoomListSnapshotMinPacketSize = HeaderSize + 2;
        public const int RoomListEntrySize = 9;
        public const int RoomListEntryMinSize = RoomListEntrySize + 1;
        public const int CreateRoomTitleMaxBytes = 64;
        public const int CreateRoomTitleMaxVisibleCharacters = 20;
        private const int NicknameMinVisibleCharacters = 2;
        private const int NicknameMaxVisibleCharacters = 12;
        public const int CreateRoomMinCapacity = 2;
        public const int CreateRoomMaxCapacity = 10;
        public const int RoomDetailStateMinPacketSize = HeaderSize + 4 + 1 + 1 + 1 + 1 + 2 + 1;
        public const int RoomDetailMemberMinSize = 4 + 1 + 1;
        public const int RoomDetailTargetActionEntrySize = 4 + 2;
        public const int LobbyReturnVisibilityPacketSize = HeaderSize + 4 + 1;
        public const int HostKickRequestPacketSize = HeaderSize + 4;
        public const int HostKickResponsePacketSize = HeaderSize + 4 + 4;
        public const int ErrorPacketSize = HeaderSize + 2 + 2;
        public const ushort WelcomePacketType = 0x0001;
        public const ushort ClientListSnapshotPacketType = 0x0002;
        public const ushort AuthenticateGameSessionPacketType = 0x0003;
        public const ushort SessionReplacedPacketType = 0x0004;
        public const ushort HeartbeatRequestPacketType = 0x0005;
        public const ushort CreateRoomRequestPacketType = 0x0101;
        public const ushort CreateRoomResponsePacketType = 0x0102;
        public const ushort JoinRoomRequestPacketType = 0x0103;
        public const ushort JoinRoomResponsePacketType = 0x0104;
        public const ushort LeaveRoomRequestPacketType = 0x0105;
        public const ushort LeaveRoomResponsePacketType = 0x0106;
        public const ushort RoomListSnapshotPacketType = 0x0107;
        public const ushort ReadyRoomRequestPacketType = 0x0108;
        public const ushort ReadyRoomResponsePacketType = 0x0109;
        public const ushort BattleStartPacketType = 0x010A;
        public const ushort MonsterSpawnPacketType = 0x010B;
        public const ushort MonsterDeathPacketType = 0x010D;
        public const ushort ClickLootRequestPacketType = 0x010F;
        public const ushort LootResolvedPacketType = 0x0110;
        public const ushort LootRejectedPacketType = 0x0111;
        public const ushort InventorySnapshotPacketType = 0x0112;
        public const ushort MonsterHealthSnapshotPacketType = 0x0119;
        public const ushort DropListSnapshotV2PacketType = 0x011A;
        public const ushort ArenaLoadCompletePacketType = 0x011B;
        public const ushort LobbyReturnVisibilityPacketType = 0x011C;
        public const ushort RoomDetailStatePacketType = 0x011D;
        public const ushort UnreadyRoomRequestPacketType = 0x011E;
        public const ushort UnreadyRoomResponsePacketType = 0x011F;
        public const ushort HostStartBattleRequestPacketType = 0x0120;
        public const ushort HostStartBattleResponsePacketType = 0x0121;
        public const ushort HostKickRequestPacketType = 0x0122;
        public const ushort HostKickResponsePacketType = 0x0123;
        public const ushort ArenaGameplayStartPacketType = 0x0124;
        public const ushort BattleFinalRankingPacketType = 0x0125;
        public const ushort BattleLoadEntryPacketType = 0x0126;
        public const ushort ErrorPacketType = 0x01FF;
        public const ushort ErrorCodeFull = 0x0001;
        public const ushort ErrorCodeNotFound = 0x0002;
        public const ushort ErrorCodeAlreadyInRoom = 0x0003;
        public const ushort ErrorCodeNotInRoom = 0x0004;
        public const ushort ErrorCodeAlreadyStarted = 0x0005;
        public const ushort ErrorCodeNotHost = 0x0006;
        public const ushort ErrorCodeNotAllReady = 0x0007;
        public const ushort ErrorCodeNotEnoughPlayers = 0x0008;
        public const ushort ErrorCodeInvalidTarget = 0x0009;
        public const ushort RoomActionLeaveRoom = 0x0001;
        public const ushort RoomActionReady = 0x0002;
        public const ushort RoomActionUnready = 0x0004;
        public const ushort RoomActionHostStartBattle = 0x0008;
        public const ushort RoomActionMaskAll =
            RoomActionLeaveRoom | RoomActionReady | RoomActionUnready | RoomActionHostStartBattle;
        public const ushort TargetActionHostKick = 0x0001;
        public const ushort TargetActionMaskAll = TargetActionHostKick;

        public static byte[] SerializeAuthenticateGameSession(string gameSessionToken)
        {
            if (string.IsNullOrEmpty(gameSessionToken))
            {
                throw new ArgumentException("game session token is required", nameof(gameSessionToken));
            }

            byte[] tokenBytes;
            try
            {
                tokenBytes = StrictUtf8.GetBytes(gameSessionToken);
            }
            catch (EncoderFallbackException ex)
            {
                throw new ArgumentException("game session token must be valid UTF-8", ex);
            }

            if (tokenBytes.Length == 0 || tokenBytes.Length > GameSessionTokenMaxBytes)
            {
                throw new ArgumentException("game session token is invalid", nameof(gameSessionToken));
            }

            int packetSize = HeaderSize + GameSessionTokenLengthFieldSize + tokenBytes.Length;
            if (packetSize > MaxPacketSize)
            {
                throw new ArgumentException("game session token packet is too large", nameof(gameSessionToken));
            }

            byte[] packet = new byte[packetSize];
            WriteU16BE(packetSize, packet, 0);
            WriteU16BE(AuthenticateGameSessionPacketType, packet, 2);
            WriteU16BE(tokenBytes.Length, packet, HeaderSize);
            Buffer.BlockCopy(
                tokenBytes,
                0,
                packet,
                HeaderSize + GameSessionTokenLengthFieldSize,
                tokenBytes.Length);
            return packet;
        }

        public static byte[] SerializeHeartbeatRequest()
        {
            byte[] packet = new byte[HeaderSize];
            WriteU16BE(HeaderSize, packet, 0);
            WriteU16BE(HeartbeatRequestPacketType, packet, 2);
            return packet;
        }

        public static byte[] SerializeCreateRoomRequest()
        {
            return SerializeCreateRoomRequest("Room", CreateRoomMaxCapacity);
        }

        public static byte[] SerializeCreateRoomRequest(string roomTitle, int maxPlayers)
        {
            if (!TryGetValidatedRoomTitleBytes(roomTitle, out byte[] titleBytes))
            {
                throw new ArgumentException("room title is invalid", nameof(roomTitle));
            }
            if (maxPlayers < CreateRoomMinCapacity || maxPlayers > CreateRoomMaxCapacity)
            {
                throw new ArgumentOutOfRangeException(
                    nameof(maxPlayers),
                    "max players must be between 2 and 10");
            }

            int packetSize = HeaderSize + 1 + titleBytes.Length + 1;
            byte[] packet = new byte[packetSize];
            WriteU16BE((ushort)packetSize, packet, 0);
            WriteU16BE(CreateRoomRequestPacketType, packet, 2);
            packet[HeaderSize] = (byte)titleBytes.Length;
            Buffer.BlockCopy(titleBytes, 0, packet, HeaderSize + 1, titleBytes.Length);
            packet[packet.Length - 1] = (byte)maxPlayers;
            return packet;
        }

        private static bool TryGetValidatedRoomTitleBytes(string roomTitle, out byte[] titleBytes)
        {
            titleBytes = null;
            if (string.IsNullOrEmpty(roomTitle))
            {
                return false;
            }

            if (new StringInfo(roomTitle).LengthInTextElements > CreateRoomTitleMaxVisibleCharacters)
            {
                return false;
            }

            bool previousWhitespace = false;
            for (int i = 0; i < roomTitle.Length; ++i)
            {
                char ch = roomTitle[i];
                if (char.IsControl(ch))
                {
                    return false;
                }

                bool whitespace = char.IsWhiteSpace(ch);
                if ((i == 0 && whitespace) || (previousWhitespace && whitespace))
                {
                    return false;
                }
                previousWhitespace = whitespace;
            }
            if (previousWhitespace)
            {
                return false;
            }

            try
            {
                byte[] encoded = StrictUtf8.GetBytes(roomTitle);
                if (encoded.Length == 0 || encoded.Length > CreateRoomTitleMaxBytes)
                {
                    return false;
                }
                titleBytes = encoded;
                return true;
            }
            catch (EncoderFallbackException)
            {
                return false;
            }
        }

        public static byte[] SerializeJoinRoomRequest(uint roomId)
        {
            if (roomId == 0U)
            {
                throw new ArgumentOutOfRangeException(nameof(roomId), "room id must be non-zero");
            }

            byte[] packet = new byte[RoomIdPacketSize];
            WriteU16BE(RoomIdPacketSize, packet, 0);
            WriteU16BE(JoinRoomRequestPacketType, packet, 2);
            WriteU32BE(roomId, packet, HeaderSize);
            return packet;
        }

        public static byte[] SerializeLeaveRoomRequest()
        {
            byte[] packet = new byte[HeaderSize];
            WriteU16BE(HeaderSize, packet, 0);
            WriteU16BE(LeaveRoomRequestPacketType, packet, 2);
            return packet;
        }

        public static byte[] SerializeReadyRoomRequest()
        {
            byte[] packet = new byte[HeaderSize];
            WriteU16BE(HeaderSize, packet, 0);
            WriteU16BE(ReadyRoomRequestPacketType, packet, 2);
            return packet;
        }

        public static byte[] SerializeUnreadyRoomRequest()
        {
            byte[] packet = new byte[HeaderSize];
            WriteU16BE(HeaderSize, packet, 0);
            WriteU16BE(UnreadyRoomRequestPacketType, packet, 2);
            return packet;
        }

        public static byte[] SerializeHostStartBattleRequest()
        {
            byte[] packet = new byte[HeaderSize];
            WriteU16BE(HeaderSize, packet, 0);
            WriteU16BE(HostStartBattleRequestPacketType, packet, 2);
            return packet;
        }

        public static byte[] SerializeHostKickRequest(uint targetSessionId)
        {
            if (targetSessionId == 0U)
            {
                throw new ArgumentOutOfRangeException(
                    nameof(targetSessionId),
                    "target session id must be non-zero");
            }

            byte[] packet = new byte[HostKickRequestPacketSize];
            WriteU16BE(HostKickRequestPacketSize, packet, 0);
            WriteU16BE(HostKickRequestPacketType, packet, 2);
            WriteU32BE(targetSessionId, packet, HeaderSize);
            return packet;
        }

        public static byte[] SerializeArenaLoadComplete(uint roomId, ulong battleInstanceId)
        {
            if (roomId == 0U)
            {
                throw new ArgumentOutOfRangeException(nameof(roomId), "room id must be non-zero");
            }

            if (battleInstanceId == 0UL)
            {
                throw new ArgumentOutOfRangeException(
                    nameof(battleInstanceId),
                    "battle instance id must be non-zero");
            }

            byte[] packet = new byte[ArenaLoadCompletePacketSize];
            WriteU16BE(ArenaLoadCompletePacketSize, packet, 0);
            WriteU16BE(ArenaLoadCompletePacketType, packet, 2);
            WriteU32BE(roomId, packet, HeaderSize);
            WriteU64BE(battleInstanceId, packet, HeaderSize + 4);
            return packet;
        }

        public static bool TryParseWelcome(byte[] packet, out ulong sessionId)
        {
            sessionId = 0UL;
            if (packet == null || packet.Length != WelcomePacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != WelcomePacketSize || type != WelcomePacketType)
            {
                return false;
            }

            sessionId = ReadU64BE(packet, HeaderSize);
            return sessionId != 0UL;
        }

        public static bool TryParseSessionReplaced(byte[] packet)
        {
            if (packet == null || packet.Length != HeaderSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            return size == HeaderSize && type == SessionReplacedPacketType;
        }

        public static bool TryParseCreateRoomResponse(
            byte[] packet,
            out uint roomId,
            out ushort playerCount)
        {
            return TryParseRoomStatus(
                packet,
                CreateRoomResponsePacketType,
                out roomId,
                out playerCount);
        }

        public static bool TryParseJoinRoomResponse(
            byte[] packet,
            out uint roomId,
            out ushort playerCount)
        {
            return TryParseRoomStatus(
                packet,
                JoinRoomResponsePacketType,
                out roomId,
                out playerCount);
        }

        public static bool TryParseLeaveRoomResponse(byte[] packet, out uint roomId)
        {
            roomId = 0U;
            if (packet == null || packet.Length != RoomIdPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != LeaveRoomResponsePacketType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            return roomId != 0U;
        }

        public static bool TryParseReadyRoomResponse(
            byte[] packet,
            out uint roomId,
            out ushort readyPlayerCount,
            out ushort totalPlayerCount)
        {
            roomId = 0U;
            readyPlayerCount = 0;
            totalPlayerCount = 0;
            if (packet == null || packet.Length != ReadyRoomResponsePacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != ReadyRoomResponsePacketType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            readyPlayerCount = ReadU16BE(packet, HeaderSize + 4);
            totalPlayerCount = ReadU16BE(packet, HeaderSize + 4 + 2);
            return true;
        }

        public static bool TryParseUnreadyRoomResponse(byte[] packet, out uint roomId)
        {
            return TryParseRoomIdPacket(packet, UnreadyRoomResponsePacketType, out roomId);
        }

        public static bool TryParseHostStartBattleResponse(byte[] packet, out uint roomId)
        {
            return TryParseRoomIdPacket(packet, HostStartBattleResponsePacketType, out roomId);
        }

        public static bool TryParseHostKickResponse(
            byte[] packet,
            out uint roomId,
            out uint targetSessionId)
        {
            roomId = 0U;
            targetSessionId = 0U;
            if (packet == null || packet.Length != HostKickResponsePacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != HostKickResponsePacketType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            targetSessionId = ReadU32BE(packet, HeaderSize + 4);
            return roomId != 0U && targetSessionId != 0U;
        }

        public static bool TryParseLobbyReturnVisibility(
            byte[] packet,
            out uint previousRoomId,
            out PlayerLobbyReturnReason reason)
        {
            previousRoomId = 0U;
            reason = PlayerLobbyReturnReason.None;
            if (packet == null || packet.Length != LobbyReturnVisibilityPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != LobbyReturnVisibilityPacketType)
            {
                return false;
            }

            previousRoomId = ReadU32BE(packet, HeaderSize);
            reason = (PlayerLobbyReturnReason)packet[HeaderSize + 4];
            return previousRoomId != 0U && IsValidLobbyReturnReason(reason);
        }

        public static bool TryParseBattleStart(
            byte[] packet,
            out uint roomId,
            out ulong playerASessionId,
            out ulong playerBSessionId)
        {
            roomId = 0U;
            playerASessionId = 0UL;
            playerBSessionId = 0UL;
            if (packet == null || packet.Length != BattleStartPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != BattleStartPacketType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            playerASessionId = ReadU64BE(packet, HeaderSize + 4);
            playerBSessionId = ReadU64BE(packet, HeaderSize + 4 + 8);
            return true;
        }

        public static bool TryParseBattleLoadEntry(
            byte[] packet,
            out uint roomId,
            out ulong battleInstanceId,
            out ulong[] playerSessionIds)
        {
            roomId = 0U;
            battleInstanceId = 0UL;
            playerSessionIds = new ulong[0];
            if (packet == null ||
                packet.Length < BattleLoadEntryMinPacketSize ||
                packet.Length > MaxPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != BattleLoadEntryPacketType)
            {
                return false;
            }

            uint parsedRoomId = ReadU32BE(packet, HeaderSize);
            ulong parsedBattleInstanceId = ReadU64BE(packet, HeaderSize + 4);
            ushort playerCount = ReadU16BE(packet, HeaderSize + 4 + 8);
            if (playerCount < BattleLoadEntryMinPlayers ||
                playerCount > BattleLoadEntryMaxPlayers)
            {
                return false;
            }

            int expectedSize = BattleLoadEntryMinPacketSize + (playerCount * BattleLoadEntryEntrySize);
            if (expectedSize != packet.Length || expectedSize > MaxPacketSize)
            {
                return false;
            }

            ulong[] parsedPlayerSessionIds = new ulong[playerCount];
            int offset = BattleLoadEntryMinPacketSize;
            for (int index = 0; index < parsedPlayerSessionIds.Length; ++index)
            {
                parsedPlayerSessionIds[index] = ReadU64BE(packet, offset);
                offset += BattleLoadEntryEntrySize;
            }

            if (!IsValidBattleLoadEntry(
                parsedRoomId,
                parsedBattleInstanceId,
                parsedPlayerSessionIds))
            {
                return false;
            }

            roomId = parsedRoomId;
            battleInstanceId = parsedBattleInstanceId;
            playerSessionIds = parsedPlayerSessionIds;
            return true;
        }

        public static bool TryParseArenaGameplayStart(
            byte[] packet,
            out uint roomId,
            out ulong battleInstanceId)
        {
            return TryParseRoomBattleInstancePacket(
                packet,
                ArenaGameplayStartPacketType,
                out roomId,
                out battleInstanceId);
        }

        public static bool TryParseBattleFinalRanking(
            byte[] packet,
            out PlayerBattleFinalRanking ranking)
        {
            ranking = default;
            if (packet == null ||
                packet.Length < BattleFinalRankingMinPacketSize ||
                packet.Length > MaxPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != BattleFinalRankingPacketType)
            {
                return false;
            }

            int offset = HeaderSize;
            uint roomId = ReadU32BE(packet, offset);
            offset += 4;
            ulong battleInstanceId = ReadU64BE(packet, offset);
            offset += 8;
            byte rowCount = packet[offset];
            offset += 1;
            if (roomId == 0U ||
                battleInstanceId == 0UL ||
                rowCount < BattleFinalRankingMinRows ||
                rowCount > BattleFinalRankingMaxRows)
            {
                return false;
            }

            PlayerBattleFinalRankingRow[] rows = new PlayerBattleFinalRankingRow[rowCount];
            for (int index = 0; index < rowCount; ++index)
            {
                if (offset + BattleFinalRankingFixedRowSize > packet.Length)
                {
                    return false;
                }

                ushort rank = ReadU16BE(packet, offset);
                offset += 2;
                ulong sessionId = ReadU64BE(packet, offset);
                offset += 8;
                if (!TryReadUtf8Field(packet, ref offset, 32, out string nickname) ||
                    offset + 8 > packet.Length)
                {
                    return false;
                }

                long totalAssetValue = ReadI64BE(packet, offset);
                offset += 8;
                if (rank == 0 ||
                    sessionId == 0UL ||
                    totalAssetValue < 0L ||
                    ContainsBattleFinalRankingSession(rows, index, sessionId) ||
                    !IsValidDisplayText(nickname, 32))
                {
                    return false;
                }

                rows[index] = new PlayerBattleFinalRankingRow(
                    rank,
                    sessionId,
                    nickname,
                    totalAssetValue);
            }

            if (offset != packet.Length)
            {
                return false;
            }

            ranking = new PlayerBattleFinalRanking(roomId, battleInstanceId, rows);
            return true;
        }

        public static bool TryParseMonsterSpawn(
            byte[] packet,
            out uint roomId,
            out uint monsterId,
            out uint monsterTypeId,
            out ushort maxHp)
        {
            roomId = 0U;
            monsterId = 0U;
            monsterTypeId = 0U;
            maxHp = 0;
            if (packet == null || packet.Length != MonsterSpawnPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != MonsterSpawnPacketType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            monsterId = ReadU32BE(packet, HeaderSize + 4);
            monsterTypeId = ReadU32BE(packet, HeaderSize + 4 + 4);
            maxHp = ReadU16BE(packet, HeaderSize + 4 + 4 + 4);
            return true;
        }

        public static bool TryParseMonsterHealthSnapshot(
            byte[] packet,
            out uint roomId,
            out uint monsterId,
            out ushort currentHp,
            out ushort maxHp)
        {
            roomId = 0U;
            monsterId = 0U;
            currentHp = 0;
            maxHp = 0;
            if (packet == null || packet.Length != MonsterHealthSnapshotPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != MonsterHealthSnapshotPacketType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            monsterId = ReadU32BE(packet, HeaderSize + 4);
            currentHp = ReadU16BE(packet, HeaderSize + 4 + 4);
            maxHp = ReadU16BE(packet, HeaderSize + 4 + 4 + 2);
            return true;
        }

        public static bool TryParseMonsterDeath(
            byte[] packet,
            out uint roomId,
            out uint monsterId)
        {
            roomId = 0U;
            monsterId = 0U;
            if (packet == null || packet.Length != MonsterDeathPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != MonsterDeathPacketType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            monsterId = ReadU32BE(packet, HeaderSize + 4);
            return true;
        }

        public static bool TryParseDropListSnapshotV2(
            byte[] packet,
            out PlayerDropListSnapshotV2 snapshot)
        {
            snapshot = default;
            if (packet == null ||
                packet.Length < DropListSnapshotV2FixedPacketSize ||
                packet.Length > MaxPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != DropListSnapshotV2PacketType)
            {
                return false;
            }

            uint roomId = ReadU32BE(packet, HeaderSize);
            uint scatterSeed = ReadU32BE(packet, HeaderSize + 4);
            ushort count = ReadU16BE(packet, HeaderSize + 4 + 4);
            int expectedSize = DropListSnapshotV2FixedPacketSize + (count * DropEntryV2Size);
            if (expectedSize != packet.Length || expectedSize > MaxPacketSize)
            {
                return false;
            }

            PlayerDropEntryV2[] drops = new PlayerDropEntryV2[count];
            int offset = DropListSnapshotV2FixedPacketSize;
            for (int index = 0; index < count; ++index)
            {
                uint dropId = ReadU32BE(packet, offset);
                uint itemId = ReadU32BE(packet, offset + 4);
                ushort quantity = ReadU16BE(packet, offset + 4 + 4);
                int posX = ReadI32BE(packet, offset + 4 + 4 + 2);
                int posY = ReadI32BE(packet, offset + 4 + 4 + 2 + 4);
                if (dropId == 0U || itemId == 0U || quantity == 0)
                {
                    return false;
                }

                drops[index] = new PlayerDropEntryV2(dropId, itemId, quantity, posX, posY);
                offset += DropEntryV2Size;
            }

            snapshot = new PlayerDropListSnapshotV2(roomId, scatterSeed, drops);
            return true;
        }

        public static bool TryParseLootResolved(
            byte[] packet,
            out PlayerLootResolved result)
        {
            result = default;
            if (packet == null || packet.Length != LootResolvedPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != LootResolvedPacketType)
            {
                return false;
            }

            uint roomId = ReadU32BE(packet, HeaderSize);
            uint dropId = ReadU32BE(packet, HeaderSize + 4);
            ulong winnerSessionId = ReadU64BE(packet, HeaderSize + 4 + 4);
            uint itemId = ReadU32BE(packet, HeaderSize + 4 + 4 + 8);
            ushort quantity = ReadU16BE(packet, HeaderSize + 4 + 4 + 8 + 4);
            if (roomId == 0U ||
                dropId == 0U ||
                winnerSessionId == 0UL ||
                itemId == 0U ||
                quantity == 0)
            {
                return false;
            }

            result = new PlayerLootResolved(roomId, dropId, winnerSessionId, itemId, quantity);
            return true;
        }

        public static bool TryParseLootRejected(
            byte[] packet,
            out PlayerLootRejected result)
        {
            result = default;
            if (packet == null || packet.Length != LootRejectedPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != LootRejectedPacketType)
            {
                return false;
            }

            uint roomId = ReadU32BE(packet, HeaderSize);
            uint dropId = ReadU32BE(packet, HeaderSize + 4);
            PlayerLootRejectReason reason =
                (PlayerLootRejectReason)ReadU16BE(packet, HeaderSize + 4 + 4);
            if (roomId == 0U || dropId == 0U || !IsValidLootRejectReason(reason))
            {
                return false;
            }

            result = new PlayerLootRejected(roomId, dropId, reason);
            return true;
        }

        public static bool TryParseInventorySnapshot(
            byte[] packet,
            out PlayerInventorySnapshot snapshot)
        {
            snapshot = default;
            if (packet == null ||
                packet.Length < InventorySnapshotMinPacketSize ||
                packet.Length > MaxPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != InventorySnapshotPacketType)
            {
                return false;
            }

            ulong snapshotSessionId = ReadU64BE(packet, HeaderSize);
            ushort currentWeight = ReadU16BE(packet, HeaderSize + 8);
            ushort maxWeight = ReadU16BE(packet, HeaderSize + 8 + 2);
            ushort count = ReadU16BE(packet, HeaderSize + 8 + 2 + 2);
            int expectedSize = InventorySnapshotMinPacketSize + (count * InventoryEntrySize);
            if (expectedSize != packet.Length ||
                expectedSize > MaxPacketSize ||
                snapshotSessionId == 0UL ||
                maxWeight == 0 ||
                currentWeight > maxWeight)
            {
                return false;
            }

            PlayerInventoryEntry[] entries = new PlayerInventoryEntry[count];
            int offset = InventorySnapshotMinPacketSize;
            for (int index = 0; index < count; ++index)
            {
                uint itemId = ReadU32BE(packet, offset);
                ushort quantity = ReadU16BE(packet, offset + 4);
                if (itemId == 0U || quantity == 0)
                {
                    return false;
                }

                entries[index] = new PlayerInventoryEntry(itemId, quantity);
                offset += InventoryEntrySize;
            }

            snapshot = new PlayerInventorySnapshot(
                snapshotSessionId,
                currentWeight,
                maxWeight,
                entries);
            return true;
        }

        public static bool TryParseClientListSnapshot(
            byte[] packet,
            out PlayerClientListSnapshot snapshot)
        {
            snapshot = default;
            if (packet == null ||
                packet.Length < ClientListSnapshotMinPacketSize ||
                packet.Length > MaxPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != ClientListSnapshotPacketType)
            {
                return false;
            }

            ushort count = ReadU16BE(packet, HeaderSize);
            int expectedSize = ClientListSnapshotMinPacketSize + (count * 8);
            if (expectedSize != packet.Length || expectedSize > MaxPacketSize)
            {
                return false;
            }

            ulong[] sessionIds = new ulong[count];
            int offset = ClientListSnapshotMinPacketSize;
            for (int index = 0; index < count; ++index)
            {
                ulong sessionId = ReadU64BE(packet, offset);
                if (sessionId == 0UL || Contains(sessionIds, index, sessionId))
                {
                    return false;
                }

                sessionIds[index] = sessionId;
                offset += 8;
            }

            snapshot = new PlayerClientListSnapshot(sessionIds);
            return true;
        }

        public static bool TryParseRoomListSnapshot(
            byte[] packet,
            out PlayerRoomListSnapshot snapshot)
        {
            snapshot = default;
            if (packet == null ||
                packet.Length < RoomListSnapshotMinPacketSize ||
                packet.Length > MaxPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != RoomListSnapshotPacketType)
            {
                return false;
            }

            ushort count = ReadU16BE(packet, HeaderSize);
            int minimumExpectedSize =
                RoomListSnapshotMinPacketSize + (count * RoomListEntryMinSize);
            if (minimumExpectedSize > packet.Length || minimumExpectedSize > MaxPacketSize)
            {
                return false;
            }

            PlayerRoomListEntry[] rooms = new PlayerRoomListEntry[count];
            int offset = RoomListSnapshotMinPacketSize;
            for (int index = 0; index < count; ++index)
            {
                if (offset + RoomListEntryMinSize > packet.Length)
                {
                    return false;
                }

                uint roomId = ReadU32BE(packet, offset);
                ushort playerCount = ReadU16BE(packet, offset + 4);
                ushort maxPlayers = ReadU16BE(packet, offset + 6);
                PlayerRoomStatus status = (PlayerRoomStatus)packet[offset + 8];
                if (!IsValidRoomStatus(status))
                {
                    return false;
                }

                offset += RoomListEntrySize;
                int titleLength = packet[offset];
                ++offset;
                if (offset + titleLength > packet.Length)
                {
                    return false;
                }

                string title;
                try
                {
                    title = StrictUtf8.GetString(packet, offset, titleLength);
                }
                catch (DecoderFallbackException)
                {
                    return false;
                }
                offset += titleLength;

                rooms[index] = new PlayerRoomListEntry(
                    roomId,
                    playerCount,
                    maxPlayers,
                    status,
                    title);
            }

            if (offset != packet.Length)
            {
                return false;
            }

            snapshot = new PlayerRoomListSnapshot(rooms);
            return true;
        }

        public static bool TryParseRoomDetailState(
            byte[] packet,
            out PlayerRoomDetailState detail)
        {
            detail = default;
            if (packet == null ||
                packet.Length < RoomDetailStateMinPacketSize ||
                packet.Length > MaxPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != RoomDetailStatePacketType)
            {
                return false;
            }

            int offset = HeaderSize;
            uint roomId = ReadU32BE(packet, offset);
            offset += 4;
            PlayerRoomStatus status = (PlayerRoomStatus)packet[offset];
            offset += 1;
            if (roomId == 0U || !IsValidRoomStatus(status))
            {
                return false;
            }

            if (!TryReadUtf8Field(packet, ref offset, 64, out string title) ||
                !IsValidRoomTitle(title))
            {
                return false;
            }

            if (offset + 2 > packet.Length)
            {
                return false;
            }

            byte maxPlayers = packet[offset];
            offset += 1;
            byte memberCount = packet[offset];
            offset += 1;
            if (maxPlayers < CreateRoomMinCapacity ||
                maxPlayers > CreateRoomMaxCapacity ||
                memberCount == 0 ||
                memberCount > maxPlayers)
            {
                return false;
            }

            PlayerRoomMemberEntry[] members = new PlayerRoomMemberEntry[memberCount];
            for (int index = 0; index < memberCount; ++index)
            {
                if (offset + RoomDetailMemberMinSize > packet.Length)
                {
                    return false;
                }

                ulong memberSessionId = ReadU32BE(packet, offset);
                offset += 4;
                if (memberSessionId == 0UL ||
                    ContainsMember(members, index, memberSessionId) ||
                    !TryReadUtf8Field(packet, ref offset, 32, out string nickname) ||
                    !IsValidDisplayText(nickname, 32) ||
                    offset >= packet.Length)
                {
                    return false;
                }

                byte readyValue = packet[offset];
                offset += 1;
                if (readyValue != 0 && readyValue != 1)
                {
                    return false;
                }

                members[index] = new PlayerRoomMemberEntry(
                    memberSessionId,
                    nickname,
                    readyValue == 1);
            }

            if (offset + 3 > packet.Length)
            {
                return false;
            }

            ushort selfActionMask = ReadU16BE(packet, offset);
            offset += 2;
            if ((selfActionMask & ~RoomActionMaskAll) != 0)
            {
                return false;
            }

            byte targetActionCount = packet[offset];
            offset += 1;
            if (offset + (targetActionCount * RoomDetailTargetActionEntrySize) != packet.Length)
            {
                return false;
            }

            PlayerRoomTargetActionEntry[] targetActions =
                new PlayerRoomTargetActionEntry[targetActionCount];
            for (int index = 0; index < targetActionCount; ++index)
            {
                ulong targetSessionId = ReadU32BE(packet, offset);
                offset += 4;
                ushort targetActionMask = ReadU16BE(packet, offset);
                offset += 2;
                if (targetSessionId == 0UL ||
                    !ContainsMember(members, memberCount, targetSessionId) ||
                    ContainsTargetAction(targetActions, index, targetSessionId) ||
                    targetActionMask == 0 ||
                    (targetActionMask & ~TargetActionMaskAll) != 0)
                {
                    return false;
                }

                targetActions[index] =
                    new PlayerRoomTargetActionEntry(targetSessionId, targetActionMask);
            }

            detail = new PlayerRoomDetailState(
                roomId,
                status,
                title,
                maxPlayers,
                members,
                selfActionMask,
                targetActions);
            return true;
        }

        public static bool TryParseError(
            byte[] packet,
            out ushort failedType,
            out ushort errorCode)
        {
            failedType = 0;
            errorCode = 0;
            if (packet == null || packet.Length != ErrorPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != ErrorPacketType)
            {
                return false;
            }

            failedType = ReadU16BE(packet, HeaderSize);
            errorCode = ReadU16BE(packet, HeaderSize + 2);
            return true;
        }

        private static void WriteU16BE(int value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 8);
            packet[offset + 1] = (byte)value;
        }

        private static void WriteU32BE(uint value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 24);
            packet[offset + 1] = (byte)(value >> 16);
            packet[offset + 2] = (byte)(value >> 8);
            packet[offset + 3] = (byte)value;
        }

        private static void WriteU64BE(ulong value, byte[] packet, int offset)
        {
            for (int index = 7; index >= 0; --index)
            {
                packet[offset + index] = (byte)value;
                value >>= 8;
            }
        }

        private static bool TryParseRoomStatus(
            byte[] packet,
            ushort expectedType,
            out uint roomId,
            out ushort playerCount)
        {
            roomId = 0U;
            playerCount = 0;
            if (packet == null || packet.Length != RoomStatusPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != expectedType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            playerCount = ReadU16BE(packet, HeaderSize + 4);
            return true;
        }

        private static bool TryParseRoomIdPacket(
            byte[] packet,
            ushort expectedType,
            out uint roomId)
        {
            roomId = 0U;
            if (packet == null || packet.Length != RoomIdPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != expectedType)
            {
                return false;
            }

            roomId = ReadU32BE(packet, HeaderSize);
            return roomId != 0U;
        }

        private static bool TryParseRoomBattleInstancePacket(
            byte[] packet,
            ushort expectedType,
            out uint roomId,
            out ulong battleInstanceId)
        {
            roomId = 0U;
            battleInstanceId = 0UL;
            if (packet == null || packet.Length != ArenaGameplayStartPacketSize)
            {
                return false;
            }

            ushort size = ReadU16BE(packet, 0);
            ushort type = ReadU16BE(packet, 2);
            if (size != packet.Length || type != expectedType)
            {
                return false;
            }

            uint parsedRoomId = ReadU32BE(packet, HeaderSize);
            ulong parsedBattleInstanceId = ReadU64BE(packet, HeaderSize + 4);
            if (parsedRoomId == 0U || parsedBattleInstanceId == 0UL)
            {
                return false;
            }

            roomId = parsedRoomId;
            battleInstanceId = parsedBattleInstanceId;
            return true;
        }

        private static bool IsValidBattleLoadEntry(
            uint roomId,
            ulong battleInstanceId,
            ulong[] playerSessionIds)
        {
            if (roomId == 0U ||
                battleInstanceId == 0UL ||
                playerSessionIds.Length < BattleLoadEntryMinPlayers ||
                playerSessionIds.Length > BattleLoadEntryMaxPlayers)
            {
                return false;
            }

            for (int index = 0; index < playerSessionIds.Length; ++index)
            {
                if (playerSessionIds[index] == 0UL)
                {
                    return false;
                }

                for (int nextIndex = index + 1; nextIndex < playerSessionIds.Length; ++nextIndex)
                {
                    if (playerSessionIds[index] == playerSessionIds[nextIndex])
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        private static bool TryReadUtf8Field(
            byte[] packet,
            ref int offset,
            int maxBytes,
            out string value)
        {
            value = string.Empty;
            if (offset >= packet.Length)
            {
                return false;
            }

            int length = packet[offset];
            offset += 1;
            if (length == 0 ||
                length > maxBytes ||
                offset + length > packet.Length)
            {
                return false;
            }

            try
            {
                value = StrictUtf8.GetString(packet, offset, length);
            }
            catch (DecoderFallbackException)
            {
                return false;
            }

            offset += length;
            return true;
        }

        private static bool IsValidDisplayText(string value, int maxBytes)
        {
            if (string.IsNullOrEmpty(value) ||
                value.Length < NicknameMinVisibleCharacters ||
                value.Length > NicknameMaxVisibleCharacters ||
                StrictUtf8.GetByteCount(value) > maxBytes)
            {
                return false;
            }

            for (int index = 0; index < value.Length; ++index)
            {
                char ch = value[index];
                if (!IsValidNicknameCharacter(ch))
                {
                    return false;
                }
            }

            return true;
        }

        private static bool IsValidNicknameCharacter(char ch)
        {
            return (ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') ||
                (ch >= '\uAC00' && ch <= '\uD7A3');
        }

        private static bool IsValidRoomTitle(string value)
        {
            return TryGetValidatedRoomTitleBytes(value, out _);
        }

        private static bool IsValidRoomStatus(PlayerRoomStatus status)
        {
            return status == PlayerRoomStatus.Open ||
                   status == PlayerRoomStatus.InProgress;
        }

        private static bool IsValidLobbyReturnReason(PlayerLobbyReturnReason reason)
        {
            return reason == PlayerLobbyReturnReason.None ||
                   reason == PlayerLobbyReturnReason.ArenaLoadTimeout ||
                   reason == PlayerLobbyReturnReason.ArenaLoadMinimumFailure ||
                   reason == PlayerLobbyReturnReason.HostKick ||
                   reason == PlayerLobbyReturnReason.ResultGenerationFailure;
        }

        private static bool ContainsMember(
            PlayerRoomMemberEntry[] members,
            int length,
            ulong sessionId)
        {
            for (int index = 0; index < length; ++index)
            {
                if (members[index].SessionId == sessionId)
                {
                    return true;
                }
            }

            return false;
        }

        private static bool ContainsTargetAction(
            PlayerRoomTargetActionEntry[] actions,
            int length,
            ulong sessionId)
        {
            for (int index = 0; index < length; ++index)
            {
                if (actions[index].TargetSessionId == sessionId)
                {
                    return true;
                }
            }

            return false;
        }

        private static ushort ReadU16BE(byte[] packet, int offset)
        {
            return (ushort)((packet[offset] << 8) | packet[offset + 1]);
        }

        private static uint ReadU32BE(byte[] packet, int offset)
        {
            return ((uint)packet[offset] << 24) |
                   ((uint)packet[offset + 1] << 16) |
                   ((uint)packet[offset + 2] << 8) |
                   packet[offset + 3];
        }

        private static int ReadI32BE(byte[] packet, int offset)
        {
            return unchecked((int)ReadU32BE(packet, offset));
        }

        private static ulong ReadU64BE(byte[] packet, int offset)
        {
            ulong value = 0UL;
            for (int index = 0; index < 8; ++index)
            {
                value = (value << 8) | packet[offset + index];
            }

            return value;
        }

        private static long ReadI64BE(byte[] packet, int offset)
        {
            return unchecked((long)ReadU64BE(packet, offset));
        }

        private static bool ContainsBattleFinalRankingSession(
            PlayerBattleFinalRankingRow[] rows,
            int length,
            ulong sessionId)
        {
            for (int index = 0; index < length; ++index)
            {
                if (rows[index].SessionId == sessionId)
                {
                    return true;
                }
            }

            return false;
        }

        private static bool Contains(ulong[] values, int length, ulong value)
        {
            for (int index = 0; index < length; ++index)
            {
                if (values[index] == value)
                {
                    return true;
                }
            }

            return false;
        }

        private static bool IsValidLootRejectReason(PlayerLootRejectReason reason)
        {
            return reason == PlayerLootRejectReason.AlreadyClaimed ||
                   reason == PlayerLootRejectReason.Overweight;
        }
    }
}
