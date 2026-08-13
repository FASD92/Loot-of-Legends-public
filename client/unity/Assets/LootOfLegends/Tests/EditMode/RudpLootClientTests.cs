using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class RudpLootClientTests
    {
        [Test]
        public void CodecMatchesFrozenLootGoldenVectors()
        {
            var header = new RudpHeader(
                RudpFlag.Reliable, 1, 2, 3, 9, 8, 15, 32);
            CollectionAssert.AreEqual(
                GoldenBytes("ClaimLootIntent"),
                RudpProtocolCodec.EncodeClaimLootIntent(
                    header,
                    new RudpCommandId(0x0102030405060708, 0x1112131415161718),
                    7,
                    2));

            Assert.That(
                RudpProtocolCodec.DecodeInbound(
                    GoldenBytes("ClaimLootTerminalResult")).Message,
                Is.TypeOf<RudpClaimLootTerminalResult>());
            Assert.That(
                RudpProtocolCodec.DecodeInbound(GoldenBytes("DropSpawned")).Message,
                Is.TypeOf<RudpDropSpawned>());
            var snapshot = (RudpDropStateSnapshot)RudpProtocolCodec.DecodeInbound(
                GoldenBytes("DropStateSnapshot")).Message;
            Assert.That(snapshot.Drops, Has.Count.EqualTo(2));
            Assert.That(snapshot.Drops[0].ItemId, Is.EqualTo(2));
            Assert.That(snapshot.Drops[1].State, Is.EqualTo(RudpLootDropState.Claimed));
            Assert.That(snapshot.Drops[1].OwnerSessionId, Is.EqualTo(1));
        }

        [Test]
        public async Task ClaimUsesOnlyServerProjectedDropIdentity()
        {
            var sender = new RecordingRudpSender();
            var input = new ClaimLootInputFacade(
                sender,
                () => new RudpHeader(RudpFlag.Reliable, 1, 2, 3, 9, 8, 15, 32),
                7);

            RudpCommandId commandId = await input.ClaimAsync(2, CancellationToken.None);

            Assert.That(sender.Datagrams, Has.Count.EqualTo(1));
            Assert.That(commandId, Is.Not.Null);
            CollectionAssert.AreEquivalent(
                new[] { "CommandId", "BattleInstanceId", "DropId" },
                typeof(RudpClaimLootIntent).GetProperties()
                    .Select(property => property.Name));
            Assert.That(
                typeof(ClaimLootInputFacade).GetMethods(
                        BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
                    .Select(method => method.Name),
                Has.None.Matches<string>(name =>
                    name.IndexOf("Receive", StringComparison.OrdinalIgnoreCase) >= 0));
        }

        [Test]
        public async Task ReliableClaimRetriesTheSameDatagramAndRetiresOnAckBits()
        {
            var sender = new RecordingRudpSender();
            long now = 0;
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(sender, pump, () => now);
                reliable.AcceptTransportEpoch(3);
                var input = new ClaimLootInputFacade(reliable, 7);

                RudpCommandId commandId = await input.ClaimAsync(
                    2, CancellationToken.None);
                now = 200;
                await reliable.TickAsync(CancellationToken.None);

                Assert.That(sender.Datagrams, Has.Count.EqualTo(2));
                CollectionAssert.AreEqual(sender.Datagrams[0], sender.Datagrams[1]);
                Assert.That(ReadUInt64(sender.Datagrams[0], 48), Is.EqualTo(commandId.High));
                Assert.That(ReadUInt64(sender.Datagrams[0], 56), Is.EqualTo(commandId.Low));

                reliable.Acknowledge(2, 1);
                now = 600;
                await reliable.TickAsync(CancellationToken.None);
                Assert.That(sender.Datagrams, Has.Count.EqualTo(2));
                Assert.That(reliable.PendingReliableCount, Is.Zero);
            }
        }

        [Test]
        public async Task ExistingInboundPumpOwnsLootReceiveAndKeepsItBounded()
        {
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var serverEndpoint = (IPEndPoint)server.Client.LocalEndPoint;
                var clientEndpoint = (IPEndPoint)client.Client.LocalEndPoint;
                var pump = new RudpInboundPump(client, serverEndpoint, 1, 2);
                for (uint sequence = 1; sequence <= 257; sequence++)
                {
                    byte[] result = WithSequence(
                        GoldenBytes("ClaimLootTerminalResult"), sequence);
                    await server.SendAsync(result, result.Length, clientEndpoint);
                    Assert.That(
                        await pump.ReceiveOnceAsync(),
                        Is.EqualTo(sequence <= 256));
                }
                Assert.That(
                    pump.CreateOutboundHeader(RudpFlag.Heartbeat, 3, 1, 24).Ack,
                    Is.EqualTo(256));
                int drained = 0;
                while (pump.TryDequeueLoot(out _))
                {
                    drained++;
                }
                Assert.That(drained, Is.EqualTo(256));
            }

            Type[] rudpTypes = typeof(RudpInboundPump).Assembly.GetTypes()
                .Where(type => type.Namespace == "LootOfLegends.Transport.Rudp")
                .ToArray();
            Assert.That(
                rudpTypes.Where(type =>
                    type.Name.EndsWith("InboundPump", StringComparison.Ordinal)),
                Is.EqualTo(new[] { typeof(RudpInboundPump) }));
        }

        [Test]
        public void ReadModelUsesLatestServerSnapshotAndClaimResultDoesNotOwnDrop()
        {
            var readModel = new BattleLootReadModel(7);
            var available = new RudpDropStateSnapshot(
                7,
                9,
                RudpLootResolutionState.Open,
                new[]
                {
                    new RudpLootDropProjection(
                        2, 1, 1, 1000, -2000,
                        RudpLootDropState.Available, 0)
                });
            Assert.That(readModel.Apply(available), Is.True);
            Assert.That(readModel.Drops.Single().State, Is.EqualTo(RudpLootDropState.Available));

            var commandId = new RudpCommandId(1, 2);
            Assert.That(
                readModel.Apply(new RudpClaimLootTerminalResult(
                    commandId, 7, 2, RudpClaimLootResultCode.Ok)),
                Is.True);
            Assert.That(readModel.Drops.Single().State, Is.EqualTo(RudpLootDropState.Available));
            Assert.That(readModel.LastClaimCommandId, Is.SameAs(commandId));
            Assert.That(readModel.LastClaimResult, Is.EqualTo(RudpClaimLootResultCode.Ok));

            Assert.That(
                readModel.Apply(new RudpDropStateSnapshot(
                    7,
                    8,
                    RudpLootResolutionState.Resolved,
                    new[]
                    {
                        new RudpLootDropProjection(
                            2, 1, 1, 1000, -2000,
                            RudpLootDropState.Claimed, 1)
                    })),
                Is.False);
            Assert.That(readModel.Drops.Single().State, Is.EqualTo(RudpLootDropState.Available));
        }

        private static byte[] GoldenBytes(string semanticName)
        {
            string path = Path.GetFullPath(Path.Combine(
                Application.dataPath,
                "../../../contracts/protocol/golden/loot-v1.json"));
            string contract = File.ReadAllText(path);
            string semanticMarker = $"\"semanticName\": \"{semanticName}\"";
            int semanticOffset = contract.IndexOf(semanticMarker, StringComparison.Ordinal);
            Assert.That(semanticOffset, Is.GreaterThanOrEqualTo(0));
            const string valueMarker = "\"datagramHex\": \"";
            int valueOffset = contract.IndexOf(valueMarker, semanticOffset, StringComparison.Ordinal);
            Assert.That(valueOffset, Is.GreaterThanOrEqualTo(0));
            int first = valueOffset + valueMarker.Length;
            int last = contract.IndexOf('"', first);
            string hex = contract.Substring(first, last - first);
            var bytes = new byte[hex.Length / 2];
            for (int index = 0; index < bytes.Length; index++)
            {
                bytes[index] = Convert.ToByte(hex.Substring(index * 2, 2), 16);
            }
            return bytes;
        }

        private static byte[] WithSequence(byte[] datagram, uint sequence)
        {
            WriteUInt32(datagram, 28, sequence);
            WriteUInt32(datagram, 44, 0);
            WriteUInt32(datagram, 44, Checksum(datagram));
            return datagram;
        }

        private static uint Checksum(byte[] datagram)
        {
            const uint polynomial = 0xedb88320;
            uint value = uint.MaxValue;
            foreach (byte current in datagram)
            {
                value ^= current;
                for (int bit = 0; bit < 8; bit++)
                {
                    value = (value >> 1) ^ ((value & 1) != 0 ? polynomial : 0);
                }
            }
            return value ^ uint.MaxValue;
        }

        private static void WriteUInt32(byte[] bytes, int offset, uint value)
        {
            bytes[offset] = (byte)(value >> 24);
            bytes[offset + 1] = (byte)(value >> 16);
            bytes[offset + 2] = (byte)(value >> 8);
            bytes[offset + 3] = (byte)value;
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

        private sealed class RecordingRudpSender : IRudpDatagramSender
        {
            public List<byte[]> Datagrams { get; } = new List<byte[]>();

            public Task SendAsync(byte[] datagram, CancellationToken cancellationToken)
            {
                Datagrams.Add(datagram);
                return Task.CompletedTask;
            }
        }
    }
}
