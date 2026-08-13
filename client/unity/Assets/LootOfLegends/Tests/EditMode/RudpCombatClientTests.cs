using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class RudpCombatClientTests
    {
        [Test]
        public void CodecMatchesFrozenCombatGoldenVectors()
        {
            var header = new RudpHeader(
                RudpFlag.Reliable, 1, 2, 3, 4, 3, 3, 27);
            CollectionAssert.AreEqual(
                GoldenBytes("AttackIntent"),
                RudpProtocolCodec.EncodeAttackIntent(
                    header,
                    new RudpCommandId(0x0102030405060708, 0x1112131415161718),
                    7,
                    1));

            Assert.That(
                RudpProtocolCodec.DecodeInbound(GoldenBytes("AttackTerminalResult")).Message,
                Is.TypeOf<RudpAttackTerminalResult>());
            Assert.That(
                RudpProtocolCodec.DecodeInbound(GoldenBytes("MonsterSpawned")).Message,
                Is.TypeOf<RudpMonsterSpawned>());
            Assert.That(
                RudpProtocolCodec.DecodeInbound(GoldenBytes("CombatTerminalEvent")).Message,
                Is.TypeOf<RudpCombatTerminalEvent>());
            Assert.That(
                RudpProtocolCodec.DecodeInbound(GoldenBytes("MonsterStateSnapshot")).Message,
                Is.TypeOf<RudpMonsterStateSnapshot>());
        }

        [Test]
        public async Task AttackIntentDoesNotChangeHitPointsBeforeServerResult()
        {
            var sender = new RecordingRudpSender();
            var readModel = new BattleCombatReadModel(7);
            Assert.That(
                readModel.Apply(
                    RudpProtocolCodec.DecodeInbound(GoldenBytes("MonsterSpawned")).Message),
                Is.True);
            var input = new AttackInputFacade(
                sender,
                () => new RudpHeader(RudpFlag.Reliable, 1, 2, 3, 9, 8, 0, 27),
                7);

            await input.AttackAsync(1, CancellationToken.None);

            Assert.That(sender.Datagrams, Has.Count.EqualTo(1));
            Assert.That(readModel.HitPoints, Is.EqualTo(1600));
            Assert.That(readModel.LastAttackResult, Is.Null);

            Assert.That(
                readModel.Apply(
                    RudpProtocolCodec.DecodeInbound(
                        GoldenBytes("AttackTerminalResult")).Message),
                Is.True);
            Assert.That(readModel.HitPoints, Is.EqualTo(1580));
            Assert.That(readModel.LastAttackResult, Is.EqualTo(RudpAttackResultCode.Ok));
            Assert.That(readModel.LastAttackCommandId.High, Is.EqualTo(0x0102030405060708));
            Assert.That(readModel.LastAttackCommandId.Low, Is.EqualTo(0x1112131415161718));
            readModel.Apply(new RudpAttackTerminalResult(
                new RudpCommandId(1, 2), 7, RudpAttackResultCode.Ok, 1, 1600, 1,
                RudpCombatOutcome.None));
            Assert.That(readModel.HitPoints, Is.EqualTo(1580));
        }

        [Test]
        public async Task ReliableAttackRetriesTheSameDatagramAndRetiresOnAck()
        {
            var sender = new RecordingRudpSender();
            long now = 0;
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    (IPEndPoint)server.Client.LocalEndPoint,
                    1,
                    2);
                var reliable = new RudpReliableOutbound(sender, pump, () => now);
                reliable.AcceptTransportEpoch(3);
                var input = new AttackInputFacade(reliable, 7);

                RudpCommandId commandId = await input.AttackAsync(
                    1, CancellationToken.None);
                now = 200;
                await reliable.TickAsync(CancellationToken.None);

                Assert.That(sender.Datagrams, Has.Count.EqualTo(2));
                CollectionAssert.AreEqual(sender.Datagrams[0], sender.Datagrams[1]);
                Assert.That(ReadUInt64(sender.Datagrams[0], 48), Is.EqualTo(commandId.High));
                Assert.That(ReadUInt64(sender.Datagrams[0], 56), Is.EqualTo(commandId.Low));

                byte[] acknowledged = WithAck(
                    GoldenBytes("AttackTerminalResult"), 1, 0);
                var clientEndpoint = (IPEndPoint)socket.Client.LocalEndPoint;
                await server.SendAsync(
                    acknowledged, acknowledged.Length, clientEndpoint);
                Assert.That(await pump.ReceiveOnceAsync(), Is.True);
                now = 600;
                await reliable.TickAsync(CancellationToken.None);
                Assert.That(sender.Datagrams, Has.Count.EqualTo(2));
                Assert.That(reliable.PendingReliableCount, Is.Zero);
            }
        }

        [Test]
        public async Task ReliableApplicationQueuePreservesTheControlReserve()
        {
            var sender = new RecordingRudpSender();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(sender, pump, () => 0);
                reliable.AcceptTransportEpoch(3);
                var input = new AttackInputFacade(reliable, 7);

                for (int index = 0; index < 224; index++)
                {
                    await input.AttackAsync(1, CancellationToken.None);
                }
                Assert.That(reliable.PendingReliableCount, Is.EqualTo(224));
                Assert.ThrowsAsync<InvalidOperationException>(async () =>
                    await input.AttackAsync(1, CancellationToken.None));
                Assert.That(reliable.HasConfirmedFailure, Is.True);
            }
        }

        [Test]
        public void MonsterSnapshotUsesOnlyTheLatestServerSequence()
        {
            var readModel = new BattleCombatReadModel(7);

            Assert.That(
                readModel.Apply(new RudpMonsterStateSnapshot(
                    7, 10, 20, 1, 1580, RudpMonsterState.Alive)),
                Is.True);
            Assert.That(
                readModel.Apply(new RudpMonsterStateSnapshot(
                    7, 9, 19, 1, 1600, RudpMonsterState.Alive)),
                Is.False);
            Assert.That(
                readModel.Apply(new RudpMonsterStateSnapshot(
                    7, 11, 21, 1, 1560, RudpMonsterState.Alive)),
                Is.True);
            Assert.That(readModel.HitPoints, Is.EqualTo(1560));
            Assert.That(readModel.SnapshotSequence, Is.EqualTo(11));
        }

        [Test]
        public void CombatTimeoutPreservesPositiveHitPointsAndNeverForcesDeath()
        {
            var readModel = new BattleCombatReadModel(7);
            readModel.Apply(new RudpMonsterSpawned(
                new RudpEventId(1, 1), 7, RudpEventStreamKind.CombatLifecycle,
                1, 1, 0, 0, 1600, 1));
            readModel.Apply(new RudpMonsterStateSnapshot(
                7, 1, 20, 1, 1580, RudpMonsterState.Alive));

            Assert.That(
                readModel.Apply(new RudpCombatTerminalEvent(
                    new RudpEventId(1, 2), 7, RudpEventStreamKind.CombatLifecycle,
                    2, RudpCombatOutcome.CombatTimeout, 1, 600, 1)),
                Is.True);

            Assert.That(readModel.Outcome, Is.EqualTo(RudpCombatOutcome.CombatTimeout));
            Assert.That(readModel.HitPoints, Is.EqualTo(1580));
            Assert.That(readModel.MonsterState, Is.EqualTo(RudpMonsterState.TimedOut));
            Assert.That(readModel.MonsterState, Is.Not.EqualTo(RudpMonsterState.Dead));
            Assert.That(
                readModel.Apply(new RudpMonsterStateSnapshot(
                    7, 2, 21, 1, 1560, RudpMonsterState.Alive)),
                Is.False);
            Assert.That(readModel.HitPoints, Is.EqualTo(1580));
            Assert.That(readModel.MonsterState, Is.EqualTo(RudpMonsterState.TimedOut));
        }

        [Test]
        public async Task ExistingInboundPumpRoutesCombatWithoutAnotherReceiveOwner()
        {
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var serverEndpoint = (IPEndPoint)server.Client.LocalEndPoint;
                var clientEndpoint = (IPEndPoint)client.Client.LocalEndPoint;
                var pump = new RudpInboundPump(client, serverEndpoint, 1, 2);
                byte[] result = GoldenBytes("AttackTerminalResult");

                await server.SendAsync(result, result.Length, clientEndpoint);
                Assert.That(await pump.ReceiveOnceAsync(), Is.True);
                Assert.That(pump.TryDequeueCombat(out RudpInboundDatagram datagram), Is.True);
                Assert.That(datagram.Message, Is.TypeOf<RudpAttackTerminalResult>());
                Assert.That(
                    pump.CreateOutboundHeader(RudpFlag.Heartbeat, 3, 1, 24).Ack,
                    Is.EqualTo(5));
            }

            Type[] rudpTypes = typeof(RudpInboundPump).Assembly.GetTypes()
                .Where(type => type.Namespace == "LootOfLegends.Transport.Rudp")
                .ToArray();
            Assert.That(
                rudpTypes.Where(type => type.Name.EndsWith("InboundPump", StringComparison.Ordinal)),
                Is.EqualTo(new[] { typeof(RudpInboundPump) }));
            Assert.That(
                typeof(AttackInputFacade).GetMethods(
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
                    .Select(method => method.Name),
                Has.None.Matches<string>(name =>
                    name.IndexOf("Receive", StringComparison.OrdinalIgnoreCase) >= 0));
        }

        [Test]
        public async Task ReorderedCombatLifecycleMatchesInOrderProjection()
        {
            var inOrder = new BattleCombatReadModel(7);
            Assert.That(
                inOrder.Apply(
                    RudpProtocolCodec.DecodeInbound(GoldenBytes("MonsterSpawned")).Message),
                Is.True);
            Assert.That(
                inOrder.Apply(
                    RudpProtocolCodec.DecodeInbound(GoldenBytes("CombatTerminalEvent")).Message),
                Is.True);

            var reordered = new BattleCombatReadModel(7);
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var serverEndpoint = (IPEndPoint)server.Client.LocalEndPoint;
                var clientEndpoint = (IPEndPoint)client.Client.LocalEndPoint;
                var pump = new RudpInboundPump(client, serverEndpoint, 1, 2);
                byte[] terminal = GoldenBytes("CombatTerminalEvent");
                byte[] spawned = GoldenBytes("MonsterSpawned");

                await server.SendAsync(terminal, terminal.Length, clientEndpoint);
                Assert.That(await pump.ReceiveOnceAsync(), Is.True);
                await server.SendAsync(spawned, spawned.Length, clientEndpoint);
                Assert.That(await pump.ReceiveOnceAsync(), Is.True);

                Assert.That(pump.TryDequeueCombat(out RudpInboundDatagram first), Is.True);
                Assert.That(first.Message, Is.TypeOf<RudpCombatTerminalEvent>());
                Assert.That(reordered.Apply(first.Message), Is.True);
                Assert.That(reordered.HasMonster, Is.False);
                Assert.That(reordered.Outcome, Is.EqualTo(RudpCombatOutcome.None));
                Assert.That(pump.TryDequeueCombat(out RudpInboundDatagram second), Is.True);
                Assert.That(second.Message, Is.TypeOf<RudpMonsterSpawned>());
                Assert.That(reordered.Apply(second.Message), Is.True);
            }

            Assert.That(reordered.HasMonster, Is.EqualTo(inOrder.HasMonster));
            Assert.That(reordered.MonsterId, Is.EqualTo(inOrder.MonsterId));
            Assert.That(reordered.HitPoints, Is.EqualTo(inOrder.HitPoints));
            Assert.That(reordered.MaximumHitPoints, Is.EqualTo(inOrder.MaximumHitPoints));
            Assert.That(reordered.MonsterState, Is.EqualTo(inOrder.MonsterState));
            Assert.That(reordered.Outcome, Is.EqualTo(inOrder.Outcome));
            Assert.That(reordered.ServerTick, Is.EqualTo(inOrder.ServerTick));
        }

        [Test]
        public async Task ReliableCombatQueueRejectsThe257thMessageBeforeAcknowledgingIt()
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
                        GoldenBytes("AttackTerminalResult"), sequence);
                    await server.SendAsync(result, result.Length, clientEndpoint);
                    Assert.That(
                        await pump.ReceiveOnceAsync(),
                        Is.EqualTo(sequence <= 256));
                }

                Assert.That(
                    pump.CreateOutboundHeader(RudpFlag.Heartbeat, 3, 1, 24).Ack,
                    Is.EqualTo(256));
                int drained = 0;
                while (pump.TryDequeueCombat(out _))
                {
                    drained++;
                }
                Assert.That(drained, Is.EqualTo(256));
            }
        }

        private static byte[] GoldenBytes(string semanticName)
        {
            string path = Path.GetFullPath(Path.Combine(
                Application.dataPath,
                "../../../contracts/protocol/golden/combat-v1.json"));
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

        private static byte[] WithAck(byte[] datagram, uint ack, uint ackBits)
        {
            WriteUInt32(datagram, 32, ack);
            WriteUInt32(datagram, 36, ackBits);
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
