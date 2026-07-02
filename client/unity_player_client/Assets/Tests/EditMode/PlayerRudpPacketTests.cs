using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerRudpPacketTests
    {
        [Test]
        public void SerializeHelloBuildsServerCompatibleDatagram()
        {
            PlayerRudpHello hello = new PlayerRudpHello(
                1,
                0xAABBCCDDU,
                0x0102030405060708UL);

            byte[] packet = PlayerRudpPacket.SerializeHello(0x01020304U, hello);

            byte[] expected =
            {
                0x4C, 0x4F, 0x01, 0x01, 0x1C, 0x01, 0x10, 0x01,
                0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0xB0, 0x17,
                0x98, 0x9E, 0x00, 0x00, 0x00, 0x01, 0xAA, 0xBB,
                0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                0x07, 0x08
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeAttackInputCommandBuildsServerCompatibleDatagram()
        {
            byte[] packet = PlayerRudpPacket.SerializeAttackInputCommand(
                0x01020304U,
                0xAABBCCDDU,
                0x10203040U,
                0x55667788U);

            byte[] expected =
            {
                0x4C, 0x4F, 0x01, 0x00, 0x1C, 0x02, 0x10, 0x02,
                0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x7E, 0x2D,
                0x45, 0x92, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD,
                0x10, 0x20, 0x30, 0x40, 0x05, 0x04, 0x55, 0x66,
                0x77, 0x88
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeAttackInputCommandAllowsZeroTargetHint()
        {
            byte[] packet = PlayerRudpPacket.SerializeAttackInputCommand(1U, 2U, 3U, 0U);

            Assert.AreEqual(0x05, packet[36]);
            Assert.AreEqual(0x04, packet[37]);
            Assert.AreEqual(0x00, packet[38]);
            Assert.AreEqual(0x00, packet[39]);
            Assert.AreEqual(0x00, packet[40]);
            Assert.AreEqual(0x00, packet[41]);
        }

        [Test]
        public void SerializeSpaceLootInputCommandBuildsServerCompatibleDatagram()
        {
            byte[] packet = PlayerRudpPacket.SerializeSpaceLootInputCommand(
                0x01020304U,
                0xAABBCCDDU,
                0x10203040U);

            byte[] expected =
            {
                0x4C, 0x4F, 0x01, 0x00, 0x1C, 0x02, 0x10, 0x02,
                0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0xE8, 0xCD,
                0x9A, 0x19, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD,
                0x10, 0x20, 0x30, 0x40, 0x06, 0x00
            };
            Assert.AreEqual(expected, packet);
        }

        [Test]
        public void SerializeSpaceLootInputCommandUsesZeroArgumentPayload()
        {
            byte[] packet = PlayerRudpPacket.SerializeSpaceLootInputCommand(1U, 2U, 3U);

            Assert.AreEqual(38, packet.Length);
            Assert.AreEqual(0x00, packet[20]);
            Assert.AreEqual(0x0A, packet[21]);
            Assert.AreEqual(0x06, packet[36]);
            Assert.AreEqual(0x00, packet[37]);
        }

        [Test]
        public void SerializeMoveInputCommandBuildsServerCompatibleDatagram()
        {
            byte[] packet = PlayerRudpPacket.SerializeMoveInputCommand(
                0x01020304U,
                0xAABBCCDDU,
                0x10203040U,
                -1000,
                1000);

            Assert.AreEqual(PlayerRudpPacket.HeaderSize + PlayerRudpPacket.MoveInputCommandPayloadSize, packet.Length);
            Assert.IsTrue(PlayerRudpPacket.TryParsePacket(
                packet,
                packet.Length,
                out PlayerRudpPacketHeader header,
                out byte[] payload));
            Assert.AreEqual(0x02, header.ChannelId);
            Assert.AreEqual(0x1002, header.PacketType);
            Assert.AreEqual(0x01020304U, header.Sequence);
            Assert.AreEqual(PlayerRudpPacket.MoveInputCommandPayloadSize, payload.Length);
            Assert.AreEqual(0xAA, payload[0]);
            Assert.AreEqual(0xBB, payload[1]);
            Assert.AreEqual(0xCC, payload[2]);
            Assert.AreEqual(0xDD, payload[3]);
            Assert.AreEqual(0x10, payload[4]);
            Assert.AreEqual(0x20, payload[5]);
            Assert.AreEqual(0x30, payload[6]);
            Assert.AreEqual(0x40, payload[7]);
            Assert.AreEqual(0x04, payload[8]);
            Assert.AreEqual(0x06, payload[9]);
            Assert.AreEqual(0xFC, payload[10]);
            Assert.AreEqual(0x18, payload[11]);
            Assert.AreEqual(0x03, payload[12]);
            Assert.AreEqual(0xE8, payload[13]);
            Assert.AreEqual(0x00, payload[14]);
            Assert.AreEqual(0x00, payload[15]);
        }

        [Test]
        public void TryParseStateSnapshotReadsServerCompatiblePayload()
        {
            byte[] payload =
            {
                0x01, 0x01,
                0x00, 0x00, 0x00, 0x11,
                0x00, 0x00, 0x00, 0x07,
                0x00, 0x02,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x03, 0xE9,
                0xFF, 0xFF, 0xFC, 0x18,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x03, 0xEA,
                0x00, 0x00, 0x03, 0xE8,
                0x00, 0x00, 0x00, 0x64
            };
            byte[] packet = BuildRudpPacket(
                flags: 0x00,
                channelId: 0x03,
                packetType: 0x1004,
                sequence: 9U,
                payload: payload);

            Assert.IsTrue(PlayerRudpPacket.TryParsePacket(
                packet,
                packet.Length,
                out PlayerRudpPacketHeader header,
                out byte[] parsedPayload));
            Assert.AreEqual(0x03, header.ChannelId);
            Assert.AreEqual(0x1004, header.PacketType);
            Assert.IsTrue(PlayerRudpPacket.TryParseStateSnapshotPayload(
                parsedPayload,
                parsedPayload.Length,
                out PlayerRudpStateSnapshot snapshot));

            Assert.AreEqual(17U, snapshot.RoomId);
            Assert.AreEqual(7U, snapshot.ServerTick);
            Assert.AreEqual(2, snapshot.Count);
            Assert.AreEqual(1001UL, snapshot.Players[0].SessionId);
            Assert.AreEqual(-1000, snapshot.Players[0].PosX);
            Assert.AreEqual(0, snapshot.Players[0].PosY);
            Assert.AreEqual(-1.0f, snapshot.Players[0].WorldX, 0.0001f);
            Assert.AreEqual(1002UL, snapshot.Players[1].SessionId);
            Assert.AreEqual(1000, snapshot.Players[1].PosX);
            Assert.AreEqual(100, snapshot.Players[1].PosY);
            Assert.AreEqual(1.0f, snapshot.Players[1].WorldX, 0.0001f);
            Assert.AreEqual(0.1f, snapshot.Players[1].WorldZ, 0.0001f);
        }

        [Test]
        public void ForSessionDerivesNonZeroClientIdFromSession()
        {
            PlayerRudpHello hello = PlayerRudpHello.ForSession(0x0102030405060708UL);

            Assert.AreEqual(1, hello.ClientVersion);
            Assert.AreEqual(0x05060708U, hello.ClientId);
            Assert.AreEqual(0x0102030405060708UL, hello.SessionId);
            Assert.IsTrue(hello.IsValid);
        }

        [Test]
        public void ForSessionRejectsZeroSession()
        {
            PlayerRudpHello hello = PlayerRudpHello.ForSession(0UL);

            Assert.IsFalse(hello.IsValid);
            Assert.AreEqual(0UL, hello.SessionId);
        }

        private static byte[] BuildRudpPacket(
            byte flags,
            byte channelId,
            ushort packetType,
            uint sequence,
            byte[] payload)
        {
            byte[] packet = new byte[PlayerRudpPacket.HeaderSize + payload.Length];
            packet[0] = 0x4C;
            packet[1] = 0x4F;
            packet[2] = 0x01;
            packet[3] = flags;
            packet[4] = PlayerRudpPacket.HeaderSize;
            packet[5] = channelId;
            WriteU16(packet, 6, packetType);
            WriteU32(packet, 8, sequence);
            WriteU32(packet, 12, 0U);
            WriteU32(packet, 16, 0U);
            WriteU16(packet, 20, (ushort)payload.Length);
            WriteU16(packet, 26, 0);
            System.Buffer.BlockCopy(payload, 0, packet, PlayerRudpPacket.HeaderSize, payload.Length);

            uint checksum = ComputeCrc32(packet, packet.Length);
            WriteU32(packet, 22, checksum);
            return packet;
        }

        private static uint ComputeCrc32(byte[] data, int size)
        {
            uint crc = 0xFFFFFFFFU;
            for (int index = 0; index < size; ++index)
            {
                crc ^= data[index];
                for (int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
                }
            }

            return crc ^ 0xFFFFFFFFU;
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
