using System;
using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerTcpPacketTests
    {
        [Test]
        public void TryParseWelcomeReadsBigEndianSessionId()
        {
            byte[] packet =
            {
                0x00, 0x0C,
                0x00, 0x01,
                0x01, 0x02, 0x03, 0x04,
                0x05, 0x06, 0x07, 0x08
            };

            bool parsed = PlayerTcpPacket.TryParseWelcome(packet, out ulong sessionId);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x0102030405060708UL, sessionId);
        }

        [Test]
        public void TryParseWelcomeRejectsWrongSizeTypeOrZeroSession()
        {
            byte[] wrongSize = { 0x00, 0x0B, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 1 };
            byte[] wrongType = { 0x00, 0x0C, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 1 };
            byte[] zeroSession = { 0x00, 0x0C, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0 };

            Assert.IsFalse(PlayerTcpPacket.TryParseWelcome(wrongSize, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseWelcome(wrongType, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseWelcome(zeroSession, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseWelcome(null, out _));
        }

        [Test]
        public void SerializeAuthenticateGameSessionWritesTokenLengthAndUtf8Token()
        {
            byte[] packet = PlayerTcpPacket.SerializeAuthenticateGameSession("dev-session:player1");

            byte[] expected =
            {
                0x00, 0x19,
                0x00, 0x03,
                0x00, 0x13,
                0x64, 0x65, 0x76, 0x2D,
                0x73, 0x65, 0x73, 0x73,
                0x69, 0x6F, 0x6E, 0x3A,
                0x70, 0x6C, 0x61, 0x79,
                0x65, 0x72, 0x31
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeAuthenticateGameSessionRejectsMissingOrOversizedToken()
        {
            Assert.Throws<ArgumentException>(
                () => PlayerTcpPacket.SerializeAuthenticateGameSession(string.Empty));
            Assert.Throws<ArgumentException>(
                () => PlayerTcpPacket.SerializeAuthenticateGameSession(null));
            Assert.Throws<ArgumentException>(
                () => PlayerTcpPacket.SerializeAuthenticateGameSession(new string('a', 513)));
        }

        [Test]
        public void SerializeHeartbeatRequestBuildsHeaderOnlyPacket()
        {
            byte[] packet = PlayerTcpPacket.SerializeHeartbeatRequest();

            byte[] expected =
            {
                0x00, 0x04,
                0x00, 0x05
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeCreateRoomRequestBuildsDefaultTitleAndCapacityPacket()
        {
            byte[] packet = PlayerTcpPacket.SerializeCreateRoomRequest();

            byte[] expected =
            {
                0x00, 0x0A,
                0x01, 0x01,
                0x04,
                0x52, 0x6F, 0x6F, 0x6D,
                0x0A
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeCreateRoomRequestWritesTitleAndCapacity()
        {
            byte[] packet = PlayerTcpPacket.SerializeCreateRoomRequest("abc", 10);

            byte[] expected =
            {
                0x00, 0x09,
                0x01, 0x01,
                0x03,
                0x61, 0x62, 0x63,
                0x0A
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeCreateRoomRequestRejectsBadLocalInput()
        {
            Assert.Throws<System.ArgumentOutOfRangeException>(
                () => PlayerTcpPacket.SerializeCreateRoomRequest("abc", 1));
            Assert.Throws<System.ArgumentOutOfRangeException>(
                () => PlayerTcpPacket.SerializeCreateRoomRequest("abc", 11));
            Assert.Throws<System.ArgumentException>(
                () => PlayerTcpPacket.SerializeCreateRoomRequest(string.Empty, 10));
            Assert.Throws<System.ArgumentException>(
                () => PlayerTcpPacket.SerializeCreateRoomRequest("bad\nroom", 10));
            Assert.Throws<System.ArgumentException>(
                () => PlayerTcpPacket.SerializeCreateRoomRequest(new string('a', 65), 10));
        }

        [Test]
        public void TryParseCreateRoomResponseReadsBigEndianRoomStatus()
        {
            byte[] packet =
            {
                0x00, 0x0A,
                0x01, 0x02,
                0x01, 0x02, 0x03, 0x04,
                0x00, 0x0A
            };

            bool parsed = PlayerTcpPacket.TryParseCreateRoomResponse(
                packet,
                out uint roomId,
                out ushort playerCount);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(10, playerCount);
        }

        [Test]
        public void TryParseCreateRoomResponseRejectsInvalidShape()
        {
            byte[] wrongSize = { 0x00, 0x09, 0x01, 0x02, 0, 0, 0, 1, 0, 1 };
            byte[] wrongType = { 0x00, 0x0A, 0x01, 0x04, 0, 0, 0, 1, 0, 1 };
            byte[] truncated = { 0x00, 0x0A, 0x01, 0x02, 0, 0, 0, 1, 0 };

            Assert.IsFalse(PlayerTcpPacket.TryParseCreateRoomResponse(wrongSize, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseCreateRoomResponse(wrongType, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseCreateRoomResponse(truncated, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseCreateRoomResponse(null, out _, out _));
        }

        [Test]
        public void SerializeJoinRoomRequestBuildsRoomIdPacket()
        {
            byte[] packet = PlayerTcpPacket.SerializeJoinRoomRequest(0x01020304U);

            byte[] expected =
            {
                0x00, 0x08,
                0x01, 0x03,
                0x01, 0x02, 0x03, 0x04
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeJoinRoomRequestRejectsZeroRoomId()
        {
            Assert.Throws<System.ArgumentOutOfRangeException>(
                () => PlayerTcpPacket.SerializeJoinRoomRequest(0U));
        }

        [Test]
        public void TryParseJoinRoomResponseReadsBigEndianRoomStatus()
        {
            byte[] packet =
            {
                0x00, 0x0A,
                0x01, 0x04,
                0x01, 0x02, 0x03, 0x04,
                0x00, 0x0A
            };

            bool parsed = PlayerTcpPacket.TryParseJoinRoomResponse(
                packet,
                out uint roomId,
                out ushort playerCount);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(10, playerCount);
        }

        [Test]
        public void TryParseJoinRoomResponseRejectsInvalidShape()
        {
            byte[] wrongSize = { 0x00, 0x09, 0x01, 0x04, 0, 0, 0, 1, 0, 1 };
            byte[] wrongType = { 0x00, 0x0A, 0x01, 0x02, 0, 0, 0, 1, 0, 1 };
            byte[] truncated = { 0x00, 0x0A, 0x01, 0x04, 0, 0, 0, 1, 0 };

            Assert.IsFalse(PlayerTcpPacket.TryParseJoinRoomResponse(wrongSize, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseJoinRoomResponse(wrongType, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseJoinRoomResponse(truncated, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseJoinRoomResponse(null, out _, out _));
        }

        [Test]
        public void SerializeReadyRoomRequestBuildsHeaderOnlyPacket()
        {
            byte[] packet = PlayerTcpPacket.SerializeReadyRoomRequest();

            byte[] expected =
            {
                0x00, 0x04,
                0x01, 0x08
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeRoomActionRequestsBuildExpectedPackets()
        {
            Assert.AreEqual(
                new byte[] { 0x00, 0x04, 0x01, 0x1E },
                PlayerTcpPacket.SerializeUnreadyRoomRequest());
            Assert.AreEqual(
                new byte[] { 0x00, 0x04, 0x01, 0x20 },
                PlayerTcpPacket.SerializeHostStartBattleRequest());
            Assert.AreEqual(
                new byte[] { 0x00, 0x08, 0x01, 0x22, 0x00, 0x00, 0x00, 0x2A },
                PlayerTcpPacket.SerializeHostKickRequest(42U));
            Assert.Throws<System.ArgumentOutOfRangeException>(
                () => PlayerTcpPacket.SerializeHostKickRequest(0U));
        }

        [Test]
        public void TryParseReadyRoomResponseReadsBigEndianReadyStatus()
        {
            byte[] packet =
            {
                0x00, 0x0C,
                0x01, 0x09,
                0x01, 0x02, 0x03, 0x04,
                0x00, 0x02,
                0x00, 0x0A
            };

            bool parsed = PlayerTcpPacket.TryParseReadyRoomResponse(
                packet,
                out uint roomId,
                out ushort readyPlayerCount,
                out ushort totalPlayerCount);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(2, readyPlayerCount);
            Assert.AreEqual(10, totalPlayerCount);
        }

        [Test]
        public void TryParseReadyRoomResponseRejectsInvalidShape()
        {
            byte[] wrongSize = { 0x00, 0x0B, 0x01, 0x09, 0, 0, 0, 1, 0, 1, 0, 2 };
            byte[] wrongType = { 0x00, 0x0C, 0x01, 0x04, 0, 0, 0, 1, 0, 1, 0, 2 };
            byte[] truncated = { 0x00, 0x0C, 0x01, 0x09, 0, 0, 0, 1, 0, 1, 0 };

            Assert.IsFalse(PlayerTcpPacket.TryParseReadyRoomResponse(wrongSize, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseReadyRoomResponse(wrongType, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseReadyRoomResponse(truncated, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseReadyRoomResponse(null, out _, out _, out _));
        }

        [Test]
        public void TryParseBattleStartReadsBigEndianPlayerSessionIds()
        {
            byte[] packet =
            {
                0x00, 0x18,
                0x01, 0x0A,
                0x01, 0x02, 0x03, 0x04,
                0x01, 0x02, 0x03, 0x04,
                0x05, 0x06, 0x07, 0x08,
                0x11, 0x12, 0x13, 0x14,
                0x15, 0x16, 0x17, 0x18
            };

            bool parsed = PlayerTcpPacket.TryParseBattleStart(
                packet,
                out uint roomId,
                out ulong playerASessionId,
                out ulong playerBSessionId);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(0x0102030405060708UL, playerASessionId);
            Assert.AreEqual(0x1112131415161718UL, playerBSessionId);
        }

        [Test]
        public void TryParseBattleStartRejectsInvalidShape()
        {
            byte[] wrongSize =
            {
                0x00, 0x17,
                0x01, 0x0A,
                0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0, 2
            };
            byte[] wrongType =
            {
                0x00, 0x18,
                0x01, 0x09,
                0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0, 2
            };
            byte[] truncated =
            {
                0x00, 0x18,
                0x01, 0x0A,
                0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0
            };

            Assert.IsFalse(PlayerTcpPacket.TryParseBattleStart(wrongSize, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleStart(wrongType, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleStart(truncated, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleStart(null, out _, out _, out _));
        }

        [Test]
        public void TryParseBattleLoadEntryReadsBigEndianRoomBattleAndRoster()
        {
            byte[] packet = BattleLoadEntryPacket(
                0x01020304U,
                0x0102030405060708UL,
                0x1112131415161718UL,
                0x2122232425262728UL,
                0x3132333435363738UL);

            bool parsed = PlayerTcpPacket.TryParseBattleLoadEntry(
                packet,
                out uint roomId,
                out ulong battleInstanceId,
                out ulong[] playerSessionIds);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(0x0102030405060708UL, battleInstanceId);
            CollectionAssert.AreEqual(
                new ulong[]
                {
                    0x1112131415161718UL,
                    0x2122232425262728UL,
                    0x3132333435363738UL
                },
                playerSessionIds);
        }

        [Test]
        public void TryParseBattleLoadEntryRejectsInvalidShape()
        {
            byte[] valid = BattleLoadEntryPacket(42U, 9001UL, 1001UL, 1002UL);

            byte[] wrongSize = (byte[])valid.Clone();
            wrongSize[1] = (byte)(wrongSize[1] - 1);

            byte[] wrongType = (byte[])valid.Clone();
            wrongType[3] = 0x18;

            byte[] truncated = new byte[valid.Length - 1];
            System.Array.Copy(valid, truncated, truncated.Length);

            byte[] zeroRoom = BattleLoadEntryPacket(0U, 9001UL, 1001UL, 1002UL);
            byte[] zeroBattle = BattleLoadEntryPacket(42U, 0UL, 1001UL, 1002UL);
            byte[] tooFew = BattleLoadEntryPacket(42U, 9001UL, 1001UL);
            ulong[] tooManySessions = new ulong[11];
            for (int index = 0; index < tooManySessions.Length; ++index)
            {
                tooManySessions[index] = (ulong)(1001 + index);
            }
            byte[] tooMany = BattleLoadEntryPacket(42U, 9001UL, tooManySessions);
            ulong[] oversizedSessions = new ulong[126];
            for (int index = 0; index < oversizedSessions.Length; ++index)
            {
                oversizedSessions[index] = (ulong)(2001 + index);
            }
            byte[] oversized = BattleLoadEntryPacket(42U, 9001UL, oversizedSessions);
            Assert.Greater(oversized.Length, PlayerTcpPacket.MaxPacketSize);
            byte[] zeroSession = BattleLoadEntryPacket(42U, 9001UL, 1001UL, 0UL);
            byte[] duplicateSession = BattleLoadEntryPacket(42U, 9001UL, 1001UL, 1001UL);

            AssertRejectsBattleLoadEntry(wrongSize);
            AssertRejectsBattleLoadEntry(wrongType);
            AssertRejectsBattleLoadEntry(truncated);
            AssertRejectsBattleLoadEntry(zeroRoom);
            AssertRejectsBattleLoadEntry(zeroBattle);
            AssertRejectsBattleLoadEntry(tooFew);
            AssertRejectsBattleLoadEntry(tooMany);
            AssertRejectsBattleLoadEntry(oversized);
            AssertRejectsBattleLoadEntry(zeroSession);
            AssertRejectsBattleLoadEntry(duplicateSession);
            AssertRejectsBattleLoadEntry(null);
        }

        [Test]
        public void SerializeArenaLoadCompleteWritesRoomAndBattleInstance()
        {
            byte[] packet = PlayerTcpPacket.SerializeArenaLoadComplete(0x01020304U, 0x0102030405060708UL);

            Assert.AreEqual(PlayerTcpPacket.ArenaLoadCompletePacketSize, packet.Length);
            Assert.AreEqual(0x00, packet[0]);
            Assert.AreEqual(0x10, packet[1]);
            Assert.AreEqual(0x01, packet[2]);
            Assert.AreEqual(0x1B, packet[3]);
            Assert.AreEqual(0x01, packet[4]);
            Assert.AreEqual(0x02, packet[5]);
            Assert.AreEqual(0x03, packet[6]);
            Assert.AreEqual(0x04, packet[7]);
            Assert.AreEqual(0x01, packet[8]);
            Assert.AreEqual(0x02, packet[9]);
            Assert.AreEqual(0x03, packet[10]);
            Assert.AreEqual(0x04, packet[11]);
            Assert.AreEqual(0x05, packet[12]);
            Assert.AreEqual(0x06, packet[13]);
            Assert.AreEqual(0x07, packet[14]);
            Assert.AreEqual(0x08, packet[15]);
        }

        [Test]
        public void SerializeArenaLoadCompleteRejectsInvalidIdentity()
        {
            Assert.Throws<ArgumentOutOfRangeException>(
                () => PlayerTcpPacket.SerializeArenaLoadComplete(0U, 9001UL));
            Assert.Throws<ArgumentOutOfRangeException>(
                () => PlayerTcpPacket.SerializeArenaLoadComplete(42U, 0UL));
        }

        [Test]
        public void TryParseArenaGameplayStartReadsBigEndianRoomAndBattle()
        {
            byte[] packet = RoomBattleInstancePacket(
                PlayerTcpPacket.ArenaGameplayStartPacketType,
                0x01020304U,
                0x0102030405060708UL);

            bool parsed = PlayerTcpPacket.TryParseArenaGameplayStart(
                packet,
                out uint roomId,
                out ulong battleInstanceId);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(0x0102030405060708UL, battleInstanceId);
        }

        [Test]
        public void TryParseArenaGameplayStartRejectsInvalidShape()
        {
            byte[] valid = RoomBattleInstancePacket(
                PlayerTcpPacket.ArenaGameplayStartPacketType,
                42U,
                9001UL);

            byte[] wrongSize = (byte[])valid.Clone();
            wrongSize[1] = (byte)(wrongSize[1] - 1);

            byte[] wrongType = (byte[])valid.Clone();
            wrongType[3] = 0x26;

            byte[] zeroRoom = RoomBattleInstancePacket(
                PlayerTcpPacket.ArenaGameplayStartPacketType,
                0U,
                9001UL);
            byte[] zeroBattle = RoomBattleInstancePacket(
                PlayerTcpPacket.ArenaGameplayStartPacketType,
                42U,
                0UL);
            byte[] truncated = new byte[valid.Length - 1];
            Array.Copy(valid, truncated, truncated.Length);

            AssertRejectsArenaGameplayStart(wrongSize);
            AssertRejectsArenaGameplayStart(wrongType);
            AssertRejectsArenaGameplayStart(zeroRoom);
            AssertRejectsArenaGameplayStart(zeroBattle);
            AssertRejectsArenaGameplayStart(truncated);
            AssertRejectsArenaGameplayStart(null);
        }

        [Test]
        public void TryParseMonsterSpawnReadsBigEndianMonsterState()
        {
            byte[] packet =
            {
                0x00, 0x12,
                0x01, 0x0B,
                0x01, 0x02, 0x03, 0x04,
                0x11, 0x22, 0x33, 0x44,
                0x55, 0x66, 0x77, 0x88,
                0x12, 0x34
            };

            bool parsed = PlayerTcpPacket.TryParseMonsterSpawn(
                packet,
                out uint roomId,
                out uint monsterId,
                out uint monsterTypeId,
                out ushort maxHp);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(0x11223344U, monsterId);
            Assert.AreEqual(0x55667788U, monsterTypeId);
            Assert.AreEqual(0x1234, maxHp);
        }

        [Test]
        public void TryParseMonsterSpawnRejectsInvalidShape()
        {
            byte[] wrongSize =
            {
                0x00, 0x11,
                0x01, 0x0B,
                0, 0, 0, 1,
                0, 0, 0, 2,
                0, 0, 0, 3,
                0, 4
            };
            byte[] wrongType =
            {
                0x00, 0x12,
                0x01, 0x0A,
                0, 0, 0, 1,
                0, 0, 0, 2,
                0, 0, 0, 3,
                0, 4
            };
            byte[] truncated =
            {
                0x00, 0x12,
                0x01, 0x0B,
                0, 0, 0, 1,
                0, 0, 0, 2,
                0, 0, 0, 3,
                0
            };

            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterSpawn(wrongSize, out _, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterSpawn(wrongType, out _, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterSpawn(truncated, out _, out _, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterSpawn(null, out _, out _, out _, out _));
        }

        [Test]
        public void TryParseMonsterHealthSnapshotReadsBigEndianHealth()
        {
            byte[] packet =
            {
                0x00, 0x10,
                0x01, 0x19,
                0x01, 0x02, 0x03, 0x04,
                0x11, 0x22, 0x33, 0x44,
                0x00, 0x4B,
                0x00, 0x64
            };

            bool parsed = PlayerTcpPacket.TryParseMonsterHealthSnapshot(
                packet,
                out uint roomId,
                out uint monsterId,
                out ushort currentHp,
                out ushort maxHp);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(0x11223344U, monsterId);
            Assert.AreEqual(75, currentHp);
            Assert.AreEqual(100, maxHp);
        }

        [Test]
        public void TryParseMonsterHealthSnapshotRejectsInvalidShape()
        {
            byte[] wrongSize =
            {
                0x00, 0x0F,
                0x01, 0x19,
                0, 0, 0, 1,
                0, 0, 0, 2,
                0, 3,
                0, 4
            };
            byte[] wrongType =
            {
                0x00, 0x10,
                0x01, 0x0B,
                0, 0, 0, 1,
                0, 0, 0, 2,
                0, 3,
                0, 4
            };
            byte[] truncated =
            {
                0x00, 0x10,
                0x01, 0x19,
                0, 0, 0, 1,
                0, 0, 0, 2,
                0, 3,
                0
            };

            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterHealthSnapshot(
                wrongSize,
                out _,
                out _,
                out _,
                out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterHealthSnapshot(
                wrongType,
                out _,
                out _,
                out _,
                out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterHealthSnapshot(
                truncated,
                out _,
                out _,
                out _,
                out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterHealthSnapshot(
                null,
                out _,
                out _,
                out _,
                out _));
        }

        [Test]
        public void TryParseMonsterDeathReadsBigEndianIdentity()
        {
            byte[] packet =
            {
                0x00, 0x0C,
                0x01, 0x0D,
                0x01, 0x02, 0x03, 0x04,
                0x11, 0x22, 0x33, 0x44
            };

            bool parsed = PlayerTcpPacket.TryParseMonsterDeath(
                packet,
                out uint roomId,
                out uint monsterId);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, roomId);
            Assert.AreEqual(0x11223344U, monsterId);
        }

        [Test]
        public void TryParseMonsterDeathRejectsInvalidShape()
        {
            byte[] wrongSize =
            {
                0x00, 0x0B,
                0x01, 0x0D,
                0, 0, 0, 1,
                0, 0, 0, 2
            };
            byte[] wrongType =
            {
                0x00, 0x0C,
                0x01, 0x19,
                0, 0, 0, 1,
                0, 0, 0, 2
            };
            byte[] truncated =
            {
                0x00, 0x0C,
                0x01, 0x0D,
                0, 0, 0, 1,
                0, 0, 0
            };

            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterDeath(wrongSize, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterDeath(wrongType, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterDeath(truncated, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseMonsterDeath(null, out _, out _));
        }

        [Test]
        public void TryParseDropListSnapshotV2ReadsSignedPositionedDrops()
        {
            byte[] packet = DropListSnapshotV2Packet(
                0x01020304U,
                0x11223344U,
                new PlayerDropEntryV2(1U, 1001U, 3, -1250, 2500),
                new PlayerDropEntryV2(2U, 1002U, 1, 0, -333));

            bool parsed = PlayerTcpPacket.TryParseDropListSnapshotV2(
                packet,
                out PlayerDropListSnapshotV2 snapshot);

            Assert.IsTrue(parsed);
            Assert.AreEqual(0x01020304U, snapshot.RoomId);
            Assert.AreEqual(0x11223344U, snapshot.ScatterSeed);
            Assert.AreEqual(2, snapshot.Count);
            Assert.AreEqual(1U, snapshot.DropAt(0).DropId);
            Assert.AreEqual(1001U, snapshot.DropAt(0).ItemId);
            Assert.AreEqual(3, snapshot.DropAt(0).Quantity);
            Assert.AreEqual(-1250, snapshot.DropAt(0).PosX);
            Assert.AreEqual(2500, snapshot.DropAt(0).PosY);
            Assert.AreEqual(2U, snapshot.DropAt(1).DropId);
            Assert.AreEqual(-333, snapshot.DropAt(1).PosY);
        }

        [Test]
        public void TryParseDropListSnapshotV2RejectsInvalidShape()
        {
            byte[] wrongSize = DropListSnapshotV2Packet(
                1U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0));
            wrongSize[1] = 0x1F;
            byte[] wrongType = DropListSnapshotV2Packet(
                1U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0));
            wrongType[3] = 0x19;
            byte[] countMismatch = DropListSnapshotV2Packet(
                1U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0));
            countMismatch[13] = 0x02;
            byte[] zeroDropId = DropListSnapshotV2Packet(
                1U,
                12345U,
                new PlayerDropEntryV2(0U, 1001U, 1, 0, 0));
            byte[] zeroItemId = DropListSnapshotV2Packet(
                1U,
                12345U,
                new PlayerDropEntryV2(1U, 0U, 1, 0, 0));
            byte[] zeroQuantity = DropListSnapshotV2Packet(
                1U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 0, 0, 0));
            byte[] oversized = new byte[1030];
            oversized[0] = 0x04;
            oversized[1] = 0x06;
            oversized[2] = 0x01;
            oversized[3] = 0x1A;

            Assert.IsFalse(PlayerTcpPacket.TryParseDropListSnapshotV2(wrongSize, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseDropListSnapshotV2(wrongType, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseDropListSnapshotV2(countMismatch, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseDropListSnapshotV2(zeroDropId, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseDropListSnapshotV2(zeroItemId, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseDropListSnapshotV2(zeroQuantity, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseDropListSnapshotV2(oversized, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseDropListSnapshotV2(null, out _));
        }

        [Test]
        public void TryParseErrorReadsReadyRoomNotInRoomErrorCode()
        {
            byte[] packet =
            {
                0x00, 0x08,
                0x01, 0xFF,
                0x01, 0x08,
                0x00, 0x04
            };

            bool parsed = PlayerTcpPacket.TryParseError(
                packet,
                out ushort failedType,
                out ushort errorCode);

            Assert.IsTrue(parsed);
            Assert.AreEqual(PlayerTcpPacket.ReadyRoomRequestPacketType, failedType);
            Assert.AreEqual(PlayerTcpPacket.ErrorCodeNotInRoom, errorCode);
        }

        [Test]
        public void TryParseErrorReadsAlreadyStartedErrorCode()
        {
            byte[] packet =
            {
                0x00, 0x08,
                0x01, 0xFF,
                0x01, 0x03,
                0x00, 0x05
            };

            bool parsed = PlayerTcpPacket.TryParseError(
                packet,
                out ushort failedType,
                out ushort errorCode);

            Assert.IsTrue(parsed);
            Assert.AreEqual(PlayerTcpPacket.JoinRoomRequestPacketType, failedType);
            Assert.AreEqual(PlayerTcpPacket.ErrorCodeAlreadyStarted, errorCode);
        }

        [Test]
        public void TryParseClientListSnapshotReadsBigEndianSessionIds()
        {
            byte[] packet =
            {
                0x00, 0x16,
                0x00, 0x02,
                0x00, 0x02,
                0x01, 0x02, 0x03, 0x04,
                0x05, 0x06, 0x07, 0x08,
                0x11, 0x12, 0x13, 0x14,
                0x15, 0x16, 0x17, 0x18
            };

            bool parsed = PlayerTcpPacket.TryParseClientListSnapshot(
                packet,
                out PlayerClientListSnapshot snapshot);

            Assert.IsTrue(parsed);
            Assert.AreEqual(2, snapshot.Count);
            Assert.AreEqual(0x0102030405060708UL, snapshot.SessionIdAt(0));
            Assert.AreEqual(0x1112131415161718UL, snapshot.SessionIdAt(1));
            Assert.IsTrue(snapshot.ContainsSession(0x0102030405060708UL));
        }

        [Test]
        public void TryParseClientListSnapshotRejectsInvalidShape()
        {
            byte[] wrongSize = { 0x00, 0x15, 0x00, 0x02, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 1 };
            byte[] wrongType = { 0x00, 0x0E, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 1 };
            byte[] countMismatch = { 0x00, 0x0E, 0x00, 0x02, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 1 };
            byte[] zeroSession = { 0x00, 0x0E, 0x00, 0x02, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0 };
            byte[] duplicateSession =
            {
                0x00, 0x16, 0x00, 0x02, 0x00, 0x02,
                0, 0, 0, 0, 0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0, 1
            };

            Assert.IsFalse(PlayerTcpPacket.TryParseClientListSnapshot(wrongSize, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseClientListSnapshot(wrongType, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseClientListSnapshot(countMismatch, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseClientListSnapshot(zeroSession, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseClientListSnapshot(duplicateSession, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseClientListSnapshot(null, out _));
        }

        [Test]
        public void TryParseRoomListSnapshotReadsBigEndianRoomEntries()
        {
            byte[] packet =
            {
                0x00, 0x23,
                0x01, 0x07,
                0x00, 0x02,
                0x00, 0x00, 0x00, 0x01,
                0x00, 0x01,
                0x00, 0x0A,
                0x00,
                0x05,
                0x41, 0x6C, 0x70, 0x68, 0x61,
                0x00, 0x00, 0x00, 0x07,
                0x00, 0x02,
                0x00, 0x0A,
                0x01,
                0x04,
                0x42, 0x6F, 0x73, 0x73
            };

            bool parsed = PlayerTcpPacket.TryParseRoomListSnapshot(
                packet,
                out PlayerRoomListSnapshot snapshot);

            Assert.IsTrue(parsed);
            Assert.AreEqual(2, snapshot.Count);
            Assert.AreEqual(1U, snapshot.RoomAt(0).RoomId);
            Assert.AreEqual(1, snapshot.RoomAt(0).PlayerCount);
            Assert.AreEqual(10, snapshot.RoomAt(0).MaxPlayers);
            Assert.AreEqual(PlayerRoomStatus.Open, snapshot.RoomAt(0).Status);
            Assert.AreEqual("Alpha", snapshot.RoomAt(0).Title);
            Assert.AreEqual(7U, snapshot.RoomAt(1).RoomId);
            Assert.AreEqual(2, snapshot.RoomAt(1).PlayerCount);
            Assert.AreEqual(10, snapshot.RoomAt(1).MaxPlayers);
            Assert.AreEqual(PlayerRoomStatus.InProgress, snapshot.RoomAt(1).Status);
            Assert.AreEqual("Boss", snapshot.RoomAt(1).Title);
            Assert.IsTrue(snapshot.ContainsRoom(7U));
        }

        [Test]
        public void TryParseRoomListSnapshotRejectsInvalidShape()
        {
            byte[] wrongSize = { 0x00, 0x17, 0x01, 0x07, 0x00, 0x02, 0, 0, 0, 1 };
            byte[] wrongType = { 0x00, 0x10, 0x01, 0x02, 0x00, 0x01, 0, 0, 0, 1, 0, 1, 0, 10, 0, 0 };
            byte[] countMismatch = { 0x00, 0x10, 0x01, 0x07, 0x00, 0x02, 0, 0, 0, 1, 0, 1, 0, 10, 0, 0 };
            byte[] unknownStatus = { 0x00, 0x10, 0x01, 0x07, 0x00, 0x01, 0, 0, 0, 1, 0, 1, 0, 10, 2, 0 };
            byte[] invalidTitleLength = { 0x00, 0x10, 0x01, 0x07, 0x00, 0x01, 0, 0, 0, 1, 0, 1, 0, 10, 0, 1 };
            byte[] invalidUtf8 = { 0x00, 0x11, 0x01, 0x07, 0x00, 0x01, 0, 0, 0, 1, 0, 1, 0, 10, 0, 1, 0xFF };
            byte[] oversized = new byte[1030];
            oversized[0] = 0x04;
            oversized[1] = 0x06;
            oversized[2] = 0x01;
            oversized[3] = 0x07;

            Assert.IsFalse(PlayerTcpPacket.TryParseRoomListSnapshot(wrongSize, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomListSnapshot(wrongType, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomListSnapshot(countMismatch, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomListSnapshot(unknownStatus, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomListSnapshot(invalidTitleLength, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomListSnapshot(invalidUtf8, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomListSnapshot(oversized, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomListSnapshot(null, out _));
        }

        [Test]
        public void SerializeLeaveRoomRequestWritesHeaderOnlyPacket()
        {
            byte[] packet = PlayerTcpPacket.SerializeLeaveRoomRequest();

            Assert.AreEqual(new byte[] { 0x00, 0x04, 0x01, 0x05 }, packet);
        }

        [Test]
        public void TryParseLeaveRoomResponseReadsRoomId()
        {
            byte[] packet = { 0x00, 0x08, 0x01, 0x06, 0x00, 0x00, 0x00, 0x11 };

            bool parsed = PlayerTcpPacket.TryParseLeaveRoomResponse(packet, out uint roomId);

            Assert.IsTrue(parsed);
            Assert.AreEqual(17U, roomId);
            Assert.IsFalse(PlayerTcpPacket.TryParseLeaveRoomResponse(
                new byte[] { 0x00, 0x08, 0x01, 0x06, 0x00, 0x00, 0x00, 0x00 },
                out _));
        }

        [Test]
        public void TryParseRoomDetailStateReadsMembersAndActions()
        {
            byte[] packet = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionHostStartBattle,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", true),
                    new PlayerRoomMemberEntry(22UL, "Player22", true)
                },
                new[]
                {
                    new PlayerRoomTargetActionEntry(22UL, PlayerTcpPacket.TargetActionHostKick)
                });

            bool parsed = PlayerTcpPacket.TryParseRoomDetailState(
                packet,
                out PlayerRoomDetailState detail);

            Assert.IsTrue(parsed);
            Assert.AreEqual(17U, detail.RoomId);
            Assert.AreEqual(PlayerRoomStatus.Open, detail.Status);
            Assert.AreEqual("Room17", detail.Title);
            Assert.AreEqual(10, detail.MaxPlayers);
            Assert.AreEqual(2, detail.MemberCount);
            Assert.AreEqual(11UL, detail.MemberAt(0).SessionId);
            Assert.AreEqual("Player11", detail.MemberAt(0).Nickname);
            Assert.IsTrue(detail.MemberAt(0).Ready);
            Assert.AreEqual(22UL, detail.MemberAt(1).SessionId);
            Assert.AreEqual(1, detail.TargetActionCount);
            Assert.AreEqual(22UL, detail.TargetActionAt(0).TargetSessionId);
            Assert.AreEqual(PlayerTcpPacket.TargetActionHostKick, detail.TargetActionAt(0).TargetActionMask);
        }

        [Test]
        public void TryParseRoomDetailStateAcceptsKoreanRoomTitle()
        {
            byte[] packet = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "한글방",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "Player11", false) },
                new PlayerRoomTargetActionEntry[0]);

            bool parsed = PlayerTcpPacket.TryParseRoomDetailState(
                packet,
                out PlayerRoomDetailState detail);

            Assert.IsTrue(parsed);
            Assert.AreEqual("한글방", detail.Title);
        }

        [Test]
        public void TryParseRoomDetailStateAcceptsKoreanNickname()
        {
            byte[] packet = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "민준", false) },
                new PlayerRoomTargetActionEntry[0]);

            bool parsed = PlayerTcpPacket.TryParseRoomDetailState(
                packet,
                out PlayerRoomDetailState detail);

            Assert.IsTrue(parsed);
            Assert.AreEqual("민준", detail.MemberAt(0).Nickname);
        }

        [Test]
        public void TryParseRoomDetailStateRejectsNicknameWithSpacesOrSymbols()
        {
            byte[] spaced = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "민 준", false) },
                new PlayerRoomTargetActionEntry[0]);
            byte[] symbol = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "민준!", false) },
                new PlayerRoomTargetActionEntry[0]);

            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(spaced, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(symbol, out _));
        }

        [Test]
        public void TryParseRoomDetailStateRejectsNicknameOutsideVisibleLengthRange()
        {
            byte[] oneCharacter = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "민", false) },
                new PlayerRoomTargetActionEntry[0]);
            byte[] thirteenCharacters = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "Player1234567", false) },
                new PlayerRoomTargetActionEntry[0]);

            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(oneCharacter, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(thirteenCharacters, out _));
        }

        [Test]
        public void TryParseRoomDetailStateRejectsMalformedFields()
        {
            byte[] unknownStatus = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "Player11", false) },
                new PlayerRoomTargetActionEntry[0]);
            unknownStatus[8] = 0x02;

            byte[] duplicateMember = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", false),
                    new PlayerRoomMemberEntry(11UL, "PlayerAgain", false)
                },
                new PlayerRoomTargetActionEntry[0]);

            byte[] reservedSelfAction = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                0x8000,
                new[] { new PlayerRoomMemberEntry(11UL, "Player11", false) },
                new PlayerRoomTargetActionEntry[0]);

            byte[] unknownReady = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "Player11", false) },
                new PlayerRoomTargetActionEntry[0]);
            unknownReady[unknownReady.Length - 4] = 0x02;

            byte[] lowCapacity = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                1,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "Player11", false) },
                new PlayerRoomTargetActionEntry[0]);

            byte[] highCapacity = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                11,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[] { new PlayerRoomMemberEntry(11UL, "Player11", false) },
                new PlayerRoomTargetActionEntry[0]);

            byte[] memberOverflow = RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                2,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", false),
                    new PlayerRoomMemberEntry(22UL, "Player22", false),
                    new PlayerRoomMemberEntry(33UL, "Player33", false)
                },
                new PlayerRoomTargetActionEntry[0]);

            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(unknownStatus, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(duplicateMember, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(reservedSelfAction, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(unknownReady, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(lowCapacity, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(highCapacity, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(memberOverflow, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseRoomDetailState(null, out _));
        }

        [Test]
        public void TryParseLobbyAndActionResponses()
        {
            Assert.IsTrue(PlayerTcpPacket.TryParseUnreadyRoomResponse(
                RoomIdPacket(PlayerTcpPacket.UnreadyRoomResponsePacketType, 17U),
                out uint unreadyRoomId));
            Assert.AreEqual(17U, unreadyRoomId);

            Assert.IsTrue(PlayerTcpPacket.TryParseHostStartBattleResponse(
                RoomIdPacket(PlayerTcpPacket.HostStartBattleResponsePacketType, 17U),
                out uint hostStartRoomId));
            Assert.AreEqual(17U, hostStartRoomId);

            Assert.IsTrue(PlayerTcpPacket.TryParseHostKickResponse(
                HostKickResponsePacket(17U, 22U),
                out uint kickRoomId,
                out uint targetSessionId));
            Assert.AreEqual(17U, kickRoomId);
            Assert.AreEqual(22U, targetSessionId);

            Assert.IsTrue(PlayerTcpPacket.TryParseLobbyReturnVisibility(
                LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.HostKick),
                out uint previousRoomId,
                out PlayerLobbyReturnReason reason));
            Assert.AreEqual(17U, previousRoomId);
            Assert.AreEqual(PlayerLobbyReturnReason.HostKick, reason);

            Assert.IsTrue(PlayerTcpPacket.TryParseLobbyReturnVisibility(
                LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.ResultGenerationFailure),
                out uint failurePreviousRoomId,
                out PlayerLobbyReturnReason failureReason));
            Assert.AreEqual(17U, failurePreviousRoomId);
            Assert.AreEqual(PlayerLobbyReturnReason.ResultGenerationFailure, failureReason);

            byte[] unknownReason = LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.HostKick);
            unknownReason[8] = 0x63;
            Assert.IsFalse(PlayerTcpPacket.TryParseLobbyReturnVisibility(unknownReason, out _, out _));
        }

        [Test]
        public void TryParseErrorReadsFailedTypeAndErrorCode()
        {
            byte[] packet =
            {
                0x00, 0x08,
                0x01, 0xFF,
                0x01, 0x01,
                0x00, 0x03
            };

            bool parsed = PlayerTcpPacket.TryParseError(
                packet,
                out ushort failedType,
                out ushort errorCode);

            Assert.IsTrue(parsed);
            Assert.AreEqual(PlayerTcpPacket.CreateRoomRequestPacketType, failedType);
            Assert.AreEqual(PlayerTcpPacket.ErrorCodeAlreadyInRoom, errorCode);
        }

        [Test]
        public void TryParseErrorRejectsInvalidShape()
        {
            byte[] wrongSize = { 0x00, 0x07, 0x01, 0xFF, 0x01, 0x01, 0x00, 0x03 };
            byte[] wrongType = { 0x00, 0x08, 0x01, 0x02, 0x01, 0x01, 0x00, 0x03 };
            byte[] truncated = { 0x00, 0x08, 0x01, 0xFF, 0x01, 0x01, 0x00 };

            Assert.IsFalse(PlayerTcpPacket.TryParseError(wrongSize, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseError(wrongType, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseError(truncated, out _, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseError(null, out _, out _));
        }

        [Test]
        public void PlayerClientListSnapshotCopiesInputAndOutputArrays()
        {
            ulong[] source = { 1UL, 2UL };
            PlayerClientListSnapshot snapshot = new PlayerClientListSnapshot(source);
            source[0] = 99UL;

            ulong[] copy = snapshot.ToArray();
            copy[1] = 88UL;

            Assert.AreEqual(1UL, snapshot.SessionIdAt(0));
            Assert.AreEqual(2UL, snapshot.SessionIdAt(1));
        }

        [Test]
        public void PlayerRoomListSnapshotCopiesInputAndOutputArrays()
        {
            PlayerRoomListEntry[] source =
            {
                new PlayerRoomListEntry(1U, 1, 10, PlayerRoomStatus.Open, "Alpha"),
                new PlayerRoomListEntry(2U, 2, 10, PlayerRoomStatus.InProgress, "Boss")
            };
            PlayerRoomListSnapshot snapshot = new PlayerRoomListSnapshot(source);
            source[0] = new PlayerRoomListEntry(99U, 9, 9);

            PlayerRoomListEntry[] copy = snapshot.ToArray();
            copy[1] = new PlayerRoomListEntry(88U, 8, 8);

            Assert.AreEqual(1U, snapshot.RoomAt(0).RoomId);
            Assert.AreEqual(2U, snapshot.RoomAt(1).RoomId);
            Assert.AreEqual(PlayerRoomStatus.InProgress, snapshot.RoomAt(1).Status);
            Assert.AreEqual("Alpha", snapshot.RoomAt(0).Title);
            Assert.AreEqual("Boss", snapshot.RoomAt(1).Title);
        }

        [Test]
        public void PlayerDropListSnapshotV2CopiesInputAndOutputArrays()
        {
            PlayerDropEntryV2[] source =
            {
                new PlayerDropEntryV2(1U, 1001U, 1, 10, 20),
                new PlayerDropEntryV2(2U, 1002U, 2, -10, -20)
            };
            PlayerDropListSnapshotV2 snapshot =
                new PlayerDropListSnapshotV2(17U, 12345U, source);
            source[0] = new PlayerDropEntryV2(99U, 9999U, 9, 0, 0);

            PlayerDropEntryV2[] copy = snapshot.ToArray();
            copy[1] = new PlayerDropEntryV2(88U, 8888U, 8, 0, 0);

            Assert.AreEqual(1U, snapshot.DropAt(0).DropId);
            Assert.AreEqual(2U, snapshot.DropAt(1).DropId);
            Assert.AreEqual(12345U, snapshot.ScatterSeed);
        }

        [Test]
        public void PlayerLootResultObjectsExposeCapturedFields()
        {
            PlayerLootResolved resolved = new PlayerLootResolved(17U, 1U, 22UL, 1001U, 1);
            PlayerLootRejected rejected =
                new PlayerLootRejected(17U, 1U, PlayerLootRejectReason.AlreadyClaimed);

            Assert.AreEqual(17U, resolved.RoomId);
            Assert.AreEqual(1U, resolved.DropId);
            Assert.AreEqual(22UL, resolved.WinnerSessionId);
            Assert.AreEqual(1001U, resolved.ItemId);
            Assert.AreEqual(1, resolved.Quantity);
            Assert.AreEqual(17U, rejected.RoomId);
            Assert.AreEqual(1U, rejected.DropId);
            Assert.AreEqual(PlayerLootRejectReason.AlreadyClaimed, rejected.Reason);
        }

        [Test]
        public void PlayerInventorySnapshotCopiesInputAndOutputArrays()
        {
            PlayerInventoryEntry[] source =
            {
                new PlayerInventoryEntry(1001U, 1),
                new PlayerInventoryEntry(1002U, 2)
            };
            PlayerInventorySnapshot snapshot =
                new PlayerInventorySnapshot(22UL, 3, 10, source);
            source[0] = new PlayerInventoryEntry(9999U, 9);

            PlayerInventoryEntry[] copy = snapshot.ToArray();
            copy[1] = new PlayerInventoryEntry(8888U, 8);

            Assert.AreEqual(22UL, snapshot.SessionId);
            Assert.AreEqual(3, snapshot.CurrentWeight);
            Assert.AreEqual(10, snapshot.MaxWeight);
            Assert.AreEqual(2, snapshot.Count);
            Assert.AreEqual(1001U, snapshot.EntryAt(0).ItemId);
            Assert.AreEqual(1, snapshot.EntryAt(0).Quantity);
            Assert.AreEqual(1002U, snapshot.EntryAt(1).ItemId);
            Assert.AreEqual(2, snapshot.EntryAt(1).Quantity);
        }

        [Test]
        public void TryParseInventorySnapshotReadsBigEndianEntries()
        {
            byte[] packet = InventorySnapshotPacket(
                22UL,
                3,
                10,
                new PlayerInventoryEntry(1001U, 1),
                new PlayerInventoryEntry(1002U, 2));

            bool parsed = PlayerTcpPacket.TryParseInventorySnapshot(
                packet,
                out PlayerInventorySnapshot snapshot);

            Assert.IsTrue(parsed);
            Assert.AreEqual(22UL, snapshot.SessionId);
            Assert.AreEqual(3, snapshot.CurrentWeight);
            Assert.AreEqual(10, snapshot.MaxWeight);
            Assert.AreEqual(2, snapshot.Count);
            Assert.AreEqual(1001U, snapshot.EntryAt(0).ItemId);
            Assert.AreEqual(1, snapshot.EntryAt(0).Quantity);
            Assert.AreEqual(1002U, snapshot.EntryAt(1).ItemId);
            Assert.AreEqual(2, snapshot.EntryAt(1).Quantity);
        }

        [Test]
        public void TryParseInventorySnapshotRejectsInvalidShape()
        {
            byte[] wrongSize = InventorySnapshotPacket(
                22UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 1));
            wrongSize[1] = 0x17;
            byte[] wrongType = InventorySnapshotPacket(
                22UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 1));
            wrongType[3] = 0x11;
            byte[] countMismatch = InventorySnapshotPacket(
                22UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 1));
            countMismatch[17] = 0x02;
            byte[] zeroSession = InventorySnapshotPacket(
                0UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 1));
            byte[] overweight = InventorySnapshotPacket(
                22UL,
                11,
                10,
                new PlayerInventoryEntry(1001U, 1));
            byte[] zeroMaxWeight = InventorySnapshotPacket(22UL, 0, 0);
            byte[] zeroItem = InventorySnapshotPacket(
                22UL,
                1,
                10,
                new PlayerInventoryEntry(0U, 1));
            byte[] zeroQuantity = InventorySnapshotPacket(
                22UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 0));
            byte[] oversized = new byte[1030];
            oversized[0] = 0x04;
            oversized[1] = 0x06;
            oversized[2] = 0x01;
            oversized[3] = 0x12;

            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(wrongSize, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(wrongType, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(countMismatch, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(zeroSession, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(overweight, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(zeroMaxWeight, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(zeroItem, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(zeroQuantity, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(oversized, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseInventorySnapshot(null, out _));
        }

        [Test]
        public void TryParseLootResolvedReadsBigEndianResult()
        {
            byte[] packet =
            {
                0x00, 0x1A,
                0x01, 0x10,
                0x00, 0x00, 0x00, 0x11,
                0x00, 0x00, 0x00, 0x01,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x16,
                0x00, 0x00, 0x03, 0xE9,
                0x00, 0x01
            };

            bool parsed = PlayerTcpPacket.TryParseLootResolved(
                packet,
                out PlayerLootResolved result);

            Assert.IsTrue(parsed);
            Assert.AreEqual(17U, result.RoomId);
            Assert.AreEqual(1U, result.DropId);
            Assert.AreEqual(22UL, result.WinnerSessionId);
            Assert.AreEqual(1001U, result.ItemId);
            Assert.AreEqual(1, result.Quantity);
        }

        [Test]
        public void TryParseLootResolvedRejectsInvalidShape()
        {
            byte[] wrongSize =
            {
                0x00, 0x19, 0x01, 0x10,
                0, 0, 0, 1,
                0, 0, 0, 2,
                0, 0, 0, 0, 0, 0, 0, 3,
                0, 0, 0, 4,
                0, 5
            };
            byte[] wrongType =
            {
                0x00, 0x1A, 0x01, 0x11,
                0, 0, 0, 1,
                0, 0, 0, 2,
                0, 0, 0, 0, 0, 0, 0, 3,
                0, 0, 0, 4,
                0, 5
            };
            byte[] zeroRoom = LootResolvedPacket(0U, 1U, 22UL, 1001U, 1);
            byte[] zeroDrop = LootResolvedPacket(17U, 0U, 22UL, 1001U, 1);
            byte[] zeroWinner = LootResolvedPacket(17U, 1U, 0UL, 1001U, 1);
            byte[] zeroItem = LootResolvedPacket(17U, 1U, 22UL, 0U, 1);
            byte[] zeroQuantity = LootResolvedPacket(17U, 1U, 22UL, 1001U, 0);

            Assert.IsFalse(PlayerTcpPacket.TryParseLootResolved(wrongSize, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootResolved(wrongType, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootResolved(zeroRoom, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootResolved(zeroDrop, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootResolved(zeroWinner, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootResolved(zeroItem, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootResolved(zeroQuantity, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootResolved(null, out _));
        }

        [Test]
        public void TryParseLootRejectedReadsAlreadyClaimedReason()
        {
            byte[] packet =
            {
                0x00, 0x0E,
                0x01, 0x11,
                0x00, 0x00, 0x00, 0x11,
                0x00, 0x00, 0x00, 0x01,
                0x00, 0x01
            };

            bool parsed = PlayerTcpPacket.TryParseLootRejected(
                packet,
                out PlayerLootRejected result);

            Assert.IsTrue(parsed);
            Assert.AreEqual(17U, result.RoomId);
            Assert.AreEqual(1U, result.DropId);
            Assert.AreEqual(PlayerLootRejectReason.AlreadyClaimed, result.Reason);
        }

        [Test]
        public void TryParseLootRejectedRejectsInvalidShape()
        {
            byte[] wrongSize = { 0x00, 0x0D, 0x01, 0x11, 0, 0, 0, 1, 0, 0, 0, 2, 0, 1 };
            byte[] wrongType = { 0x00, 0x0E, 0x01, 0x10, 0, 0, 0, 1, 0, 0, 0, 2, 0, 1 };
            byte[] zeroRoom = LootRejectedPacket(0U, 1U, 1);
            byte[] zeroDrop = LootRejectedPacket(17U, 0U, 1);
            byte[] noneReason = LootRejectedPacket(17U, 1U, 0);
            byte[] unknownReason = LootRejectedPacket(17U, 1U, 99);

            Assert.IsFalse(PlayerTcpPacket.TryParseLootRejected(wrongSize, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootRejected(wrongType, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootRejected(zeroRoom, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootRejected(zeroDrop, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootRejected(noneReason, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootRejected(unknownReason, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseLootRejected(null, out _));
        }

        [Test]
        public void TryParseBattleFinalRankingReadsServerOrderedRows()
        {
            byte[] packet = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", 500L),
                new PlayerBattleFinalRankingRow(1, 11UL, "PlayerB", 500L),
                new PlayerBattleFinalRankingRow(3, 12UL, "PlayerC", 100L));

            bool parsed = PlayerTcpPacket.TryParseBattleFinalRanking(
                packet,
                out PlayerBattleFinalRanking ranking);

            Assert.IsTrue(parsed);
            Assert.AreEqual(42U, ranking.RoomId);
            Assert.AreEqual(9001UL, ranking.BattleInstanceId);
            Assert.AreEqual(3, ranking.Count);
            Assert.AreEqual(10UL, ranking.RowAt(0).SessionId);
            Assert.AreEqual("PlayerA", ranking.RowAt(0).Nickname);
            Assert.AreEqual(500L, ranking.RowAt(0).TotalAssetValue);
            Assert.AreEqual(1, ranking.RowAt(1).Rank);
            Assert.AreEqual(3, ranking.RowAt(2).Rank);
        }

        [Test]
        public void TryParseBattleFinalRankingRejectsInvalidShape()
        {
            byte[] valid = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", 500L));
            byte[] wrongSize = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", 500L));
            wrongSize[1] = (byte)(wrongSize.Length - 1);
            byte[] wrongType = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", 500L));
            wrongType[3] = 0x24;
            byte[] zeroRoom = BattleFinalRankingPacket(
                0U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", 500L));
            byte[] zeroBattle = BattleFinalRankingPacket(
                42U,
                0UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", 500L));
            byte[] zeroRank = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(0, 10UL, "PlayerA", 500L));
            byte[] zeroSession = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 0UL, "PlayerA", 500L));
            byte[] negativeValue = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", -1L));
            byte[] duplicateSession = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", 500L),
                new PlayerBattleFinalRankingRow(2, 10UL, "PlayerB", 100L));
            byte[] zeroCount = BattleFinalRankingPacket(
                42U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 10UL, "PlayerA", 500L));
            zeroCount[16] = 0;
            byte[] truncated = new byte[valid.Length - 1];
            System.Buffer.BlockCopy(valid, 0, truncated, 0, truncated.Length);

            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(wrongSize, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(wrongType, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(zeroRoom, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(zeroBattle, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(zeroRank, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(zeroSession, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(negativeValue, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(duplicateSession, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(zeroCount, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(truncated, out _));
            Assert.IsFalse(PlayerTcpPacket.TryParseBattleFinalRanking(null, out _));
        }

        private static byte[] LootResolvedPacket(
            uint roomId,
            uint dropId,
            ulong winnerSessionId,
            uint itemId,
            ushort quantity)
        {
            byte[] packet = new byte[26];
            packet[0] = 0x00;
            packet[1] = 0x1A;
            packet[2] = 0x01;
            packet[3] = 0x10;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(dropId, packet, 8);
            WriteU64BE(winnerSessionId, packet, 12);
            WriteU32BE(itemId, packet, 20);
            packet[24] = (byte)(quantity >> 8);
            packet[25] = (byte)quantity;
            return packet;
        }

        private static byte[] LootRejectedPacket(uint roomId, uint dropId, ushort reason)
        {
            byte[] packet = new byte[14];
            packet[0] = 0x00;
            packet[1] = 0x0E;
            packet[2] = 0x01;
            packet[3] = 0x11;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(dropId, packet, 8);
            packet[12] = (byte)(reason >> 8);
            packet[13] = (byte)reason;
            return packet;
        }

        private static byte[] RoomDetailStatePacket(
            uint roomId,
            PlayerRoomStatus status,
            string title,
            byte maxPlayers,
            ushort selfActionMask,
            PlayerRoomMemberEntry[] members,
            PlayerRoomTargetActionEntry[] targetActions)
        {
            byte[] titleBytes = System.Text.Encoding.UTF8.GetBytes(title);
            int size = 4 + 4 + 1 + 1 + titleBytes.Length + 1 + 1 + 2 + 1;
            foreach (PlayerRoomMemberEntry member in members)
            {
                byte[] nicknameBytes = System.Text.Encoding.UTF8.GetBytes(member.Nickname);
                size += 4 + 1 + nicknameBytes.Length + 1;
            }
            size += targetActions.Length * 6;

            byte[] packet = new byte[size];
            packet[0] = (byte)(size >> 8);
            packet[1] = (byte)size;
            packet[2] = 0x01;
            packet[3] = 0x1D;
            int offset = 4;
            WriteU32BE(roomId, packet, offset);
            offset += 4;
            packet[offset++] = (byte)status;
            packet[offset++] = (byte)titleBytes.Length;
            System.Buffer.BlockCopy(titleBytes, 0, packet, offset, titleBytes.Length);
            offset += titleBytes.Length;
            packet[offset++] = maxPlayers;
            packet[offset++] = (byte)members.Length;
            foreach (PlayerRoomMemberEntry member in members)
            {
                byte[] nicknameBytes = System.Text.Encoding.UTF8.GetBytes(member.Nickname);
                WriteU32BE((uint)member.SessionId, packet, offset);
                offset += 4;
                packet[offset++] = (byte)nicknameBytes.Length;
                System.Buffer.BlockCopy(nicknameBytes, 0, packet, offset, nicknameBytes.Length);
                offset += nicknameBytes.Length;
                packet[offset++] = member.Ready ? (byte)1 : (byte)0;
            }
            packet[offset++] = (byte)(selfActionMask >> 8);
            packet[offset++] = (byte)selfActionMask;
            packet[offset++] = (byte)targetActions.Length;
            foreach (PlayerRoomTargetActionEntry action in targetActions)
            {
                WriteU32BE((uint)action.TargetSessionId, packet, offset);
                offset += 4;
                packet[offset++] = (byte)(action.TargetActionMask >> 8);
                packet[offset++] = (byte)action.TargetActionMask;
            }

            return packet;
        }

        private static byte[] RoomIdPacket(ushort packetType, uint roomId)
        {
            byte[] packet = new byte[8];
            packet[0] = 0x00;
            packet[1] = 0x08;
            packet[2] = (byte)(packetType >> 8);
            packet[3] = (byte)packetType;
            WriteU32BE(roomId, packet, 4);
            return packet;
        }

        private static byte[] HostKickResponsePacket(uint roomId, uint targetSessionId)
        {
            byte[] packet = new byte[12];
            packet[0] = 0x00;
            packet[1] = 0x0C;
            packet[2] = 0x01;
            packet[3] = 0x23;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(targetSessionId, packet, 8);
            return packet;
        }

        private static byte[] LobbyReturnVisibilityPacket(
            uint previousRoomId,
            PlayerLobbyReturnReason reason)
        {
            byte[] packet = new byte[9];
            packet[0] = 0x00;
            packet[1] = 0x09;
            packet[2] = 0x01;
            packet[3] = 0x1C;
            WriteU32BE(previousRoomId, packet, 4);
            packet[8] = (byte)reason;
            return packet;
        }

        private static byte[] BattleFinalRankingPacket(
            uint roomId,
            ulong battleInstanceId,
            params PlayerBattleFinalRankingRow[] rows)
        {
            int size = 17;
            foreach (PlayerBattleFinalRankingRow row in rows)
            {
                byte[] nicknameBytes = System.Text.Encoding.UTF8.GetBytes(row.Nickname);
                size += 2 + 8 + 1 + nicknameBytes.Length + 8;
            }

            byte[] packet = new byte[size];
            packet[0] = (byte)(size >> 8);
            packet[1] = (byte)size;
            packet[2] = 0x01;
            packet[3] = 0x25;
            WriteU32BE(roomId, packet, 4);
            WriteU64BE(battleInstanceId, packet, 8);
            packet[16] = (byte)rows.Length;

            int offset = 17;
            foreach (PlayerBattleFinalRankingRow row in rows)
            {
                packet[offset++] = (byte)(row.Rank >> 8);
                packet[offset++] = (byte)row.Rank;
                WriteU64BE(row.SessionId, packet, offset);
                offset += 8;
                byte[] nicknameBytes = System.Text.Encoding.UTF8.GetBytes(row.Nickname);
                packet[offset++] = (byte)nicknameBytes.Length;
                System.Buffer.BlockCopy(nicknameBytes, 0, packet, offset, nicknameBytes.Length);
                offset += nicknameBytes.Length;
                WriteI64BE(row.TotalAssetValue, packet, offset);
                offset += 8;
            }

            return packet;
        }

        private static byte[] BattleLoadEntryPacket(
            uint roomId,
            ulong battleInstanceId,
            params ulong[] playerSessionIds)
        {
            byte[] packet = new byte[18 + (playerSessionIds.Length * 8)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x26;
            WriteU32BE(roomId, packet, 4);
            WriteU64BE(battleInstanceId, packet, 8);
            packet[16] = (byte)(playerSessionIds.Length >> 8);
            packet[17] = (byte)playerSessionIds.Length;

            int offset = 18;
            foreach (ulong sessionId in playerSessionIds)
            {
                WriteU64BE(sessionId, packet, offset);
                offset += 8;
            }

            return packet;
        }

        private static byte[] RoomBattleInstancePacket(
            ushort packetType,
            uint roomId,
            ulong battleInstanceId)
        {
            byte[] packet = new byte[16];
            packet[0] = 0x00;
            packet[1] = 0x10;
            packet[2] = (byte)(packetType >> 8);
            packet[3] = (byte)packetType;
            WriteU32BE(roomId, packet, 4);
            WriteU64BE(battleInstanceId, packet, 8);
            return packet;
        }

        private static void AssertRejectsBattleLoadEntry(byte[] packet)
        {
            bool parsed = PlayerTcpPacket.TryParseBattleLoadEntry(
                packet,
                out uint roomId,
                out ulong battleInstanceId,
                out ulong[] playerSessionIds);

            Assert.IsFalse(parsed);
            Assert.AreEqual(0U, roomId);
            Assert.AreEqual(0UL, battleInstanceId);
            Assert.IsNotNull(playerSessionIds);
            Assert.AreEqual(0, playerSessionIds.Length);
        }

        private static void AssertRejectsArenaGameplayStart(byte[] packet)
        {
            bool parsed = PlayerTcpPacket.TryParseArenaGameplayStart(
                packet,
                out uint roomId,
                out ulong battleInstanceId);

            Assert.IsFalse(parsed);
            Assert.AreEqual(0U, roomId);
            Assert.AreEqual(0UL, battleInstanceId);
        }

        private static byte[] InventorySnapshotPacket(
            ulong sessionId,
            ushort currentWeight,
            ushort maxWeight,
            params PlayerInventoryEntry[] entries)
        {
            byte[] packet = new byte[18 + (entries.Length * 6)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x12;
            WriteU64BE(sessionId, packet, 4);
            packet[12] = (byte)(currentWeight >> 8);
            packet[13] = (byte)currentWeight;
            packet[14] = (byte)(maxWeight >> 8);
            packet[15] = (byte)maxWeight;
            packet[16] = (byte)(entries.Length >> 8);
            packet[17] = (byte)entries.Length;

            int offset = 18;
            foreach (PlayerInventoryEntry entry in entries)
            {
                WriteU32BE(entry.ItemId, packet, offset);
                packet[offset + 4] = (byte)(entry.Quantity >> 8);
                packet[offset + 5] = (byte)entry.Quantity;
                offset += 6;
            }

            return packet;
        }

        private static byte[] DropListSnapshotV2Packet(
            uint roomId,
            uint scatterSeed,
            params PlayerDropEntryV2[] drops)
        {
            byte[] packet = new byte[14 + (drops.Length * 18)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x1A;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(scatterSeed, packet, 8);
            packet[12] = (byte)(drops.Length >> 8);
            packet[13] = (byte)drops.Length;

            int offset = 14;
            foreach (PlayerDropEntryV2 drop in drops)
            {
                WriteU32BE(drop.DropId, packet, offset);
                WriteU32BE(drop.ItemId, packet, offset + 4);
                packet[offset + 8] = (byte)(drop.Quantity >> 8);
                packet[offset + 9] = (byte)drop.Quantity;
                WriteI32BE(drop.PosX, packet, offset + 10);
                WriteI32BE(drop.PosY, packet, offset + 14);
                offset += 18;
            }

            return packet;
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
                packet[offset + 7 - index] = (byte)(value >> (index * 8));
            }
        }

        private static void WriteI64BE(long value, byte[] packet, int offset)
        {
            WriteU64BE(unchecked((ulong)value), packet, offset);
        }

        private static void WriteI32BE(int value, byte[] packet, int offset)
        {
            WriteU32BE(unchecked((uint)value), packet, offset);
        }
    }
}
