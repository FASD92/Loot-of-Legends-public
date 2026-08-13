using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Transport;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class RudpMovementClientTests
    {
        [Test]
        public void CodecMatchesFrozenBindAndMovementGoldenVectors()
        {
            CollectionAssert.AreEqual(
                GoldenBytes("rudp-bind-v1.json", "RequestRudpBindCapability", "frameHex"),
                RudpProtocolCodec.EncodeBindCapabilityRequest(1));

            RudpBindCapability capability = RudpProtocolCodec.DecodeBindCapability(
                GoldenBytes("rudp-bind-v1.json", "RudpBindCapability", "frameHex"));
            Assert.That(capability.RequestId, Is.EqualTo(1));
            Assert.That(capability.TtlMillis, Is.EqualTo(15000));
            Assert.That(capability.Bytes, Is.EqualTo(Enumerable.Range(0, 32).Select(value => (byte)value)));

            var bindHelloHeader = new RudpHeader(
                RudpFlag.Reliable, 1, 2, 0, 1, 0, 0, 22);
            CollectionAssert.AreEqual(
                GoldenBytes("rudp-bind-v1.json", "RudpBindHello", "datagramHex"),
                RudpProtocolCodec.EncodeBindHello(bindHelloHeader, capability.Bytes));

            RudpInboundDatagram accepted = RudpProtocolCodec.DecodeInbound(
                GoldenBytes("rudp-bind-v1.json", "RudpBindAccepted", "datagramHex"));
            Assert.That(accepted.Message, Is.TypeOf<RudpBindAccepted>());
            Assert.That(accepted.Header.TransportEpoch, Is.EqualTo(3));

            var heartbeatHeader = new RudpHeader(
                RudpFlag.Heartbeat, 1, 2, 3, 2, 1, 0, 24);
            CollectionAssert.AreEqual(
                GoldenBytes("rudp-bind-v1.json", "RudpHeartbeat", "datagramHex"),
                RudpProtocolCodec.EncodeHeartbeat(heartbeatHeader));

            var moveHeader = new RudpHeader(
                RudpFlag.Unreliable, 1, 2, 3, 2, 1, 0, 25);
            CollectionAssert.AreEqual(
                GoldenBytes("movement-v1.json", "MoveIntent", "datagramHex"),
                RudpProtocolCodec.EncodeMoveIntent(
                    moveHeader, 7, 9, short.MaxValue, -short.MaxValue));

            RudpInboundDatagram snapshotDatagram = RudpProtocolCodec.DecodeInbound(
                GoldenBytes("movement-v1.json", "StateSnapshot", "datagramHex"));
            var snapshot = (RudpStateSnapshot)snapshotDatagram.Message;
            Assert.That(snapshot.BattleInstanceId, Is.EqualTo(7));
            Assert.That(snapshot.SnapshotSequence, Is.EqualTo(11));
            Assert.That(snapshot.ServerTick, Is.EqualTo(20));
            Assert.That(snapshot.Players, Has.Count.EqualTo(2));
            Assert.That(snapshot.Players[0].PositionXMillimeters, Is.EqualTo(1000));
            Assert.That(snapshot.Players[0].PositionYMillimeters, Is.EqualTo(-2000));
            Assert.That(snapshot.Players[1].PositionXMillimeters, Is.EqualTo(-10000));
            Assert.That(snapshot.Players[1].PositionYMillimeters, Is.EqualTo(10000));

            byte[] corrupt = GoldenBytes("movement-v1.json", "StateSnapshot", "datagramHex");
            corrupt[corrupt.Length - 1] ^= 1;
            Assert.Throws<RudpProtocolException>(() => RudpProtocolCodec.DecodeInbound(corrupt));
        }

        [Test]
        public async Task RudpInboundPumpIsTheOnlyRudpReceiveOwner()
        {
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var serverEndpoint = (IPEndPoint)server.Client.LocalEndPoint;
                var clientEndpoint = (IPEndPoint)client.Client.LocalEndPoint;
                var pump = new RudpInboundPump(client, serverEndpoint, 1, 2);
                byte[] snapshot = GoldenBytes(
                    "movement-v1.json", "StateSnapshot", "datagramHex");

                await server.SendAsync(snapshot, snapshot.Length, clientEndpoint);
                Assert.That(await pump.ReceiveOnceAsync(), Is.True);
                Assert.That(pump.TryDequeue(out RudpInboundDatagram datagram), Is.True);
                Assert.That(datagram.Message, Is.TypeOf<RudpStateSnapshot>());
            }

            Type[] rudpTypes = typeof(RudpInboundPump).Assembly.GetTypes()
                .Where(type => type.Namespace == "LootOfLegends.Transport.Rudp")
                .ToArray();
            Assert.That(
                rudpTypes.Where(type => type.Name.EndsWith("InboundPump", StringComparison.Ordinal)),
                Is.EqualTo(new[] { typeof(RudpInboundPump) }));
            Assert.That(
                typeof(BattleMovementClient).GetMethods(
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
                    .Select(method => method.Name),
                Has.None.Matches<string>(name =>
                    name.IndexOf("Receive", StringComparison.OrdinalIgnoreCase) >= 0));
        }

        [Test]
        public async Task RudpInboundPumpCoalescesControlAndLatestSnapshot()
        {
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var serverEndpoint = (IPEndPoint)server.Client.LocalEndPoint;
                var clientEndpoint = (IPEndPoint)client.Client.LocalEndPoint;
                var pump = new RudpInboundPump(client, serverEndpoint, 1, 2);
                byte[] accepted = GoldenBytes(
                    "rudp-bind-v1.json", "RudpBindAccepted", "datagramHex");
                byte[] snapshot = GoldenBytes(
                    "movement-v1.json", "StateSnapshot", "datagramHex");

                foreach (byte[] datagram in new[] { snapshot, accepted, snapshot, accepted })
                {
                    await server.SendAsync(datagram, datagram.Length, clientEndpoint);
                    Assert.That(await pump.ReceiveOnceAsync(), Is.True);
                }

                Assert.That(pump.TryDequeue(out RudpInboundDatagram control), Is.True);
                Assert.That(control.Message, Is.TypeOf<RudpBindAccepted>());
                Assert.That(pump.TryDequeue(out RudpInboundDatagram latest), Is.True);
                Assert.That(latest.Message, Is.TypeOf<RudpStateSnapshot>());
                Assert.That(pump.TryDequeue(out _), Is.False);
            }
        }

        [Test]
        public async Task RudpInboundPumpAcceptsPreparedRebindAndRejectsStaleEpoch()
        {
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var serverEndpoint = (IPEndPoint)server.Client.LocalEndPoint;
                var clientEndpoint = (IPEndPoint)client.Client.LocalEndPoint;
                var pump = new RudpInboundPump(client, serverEndpoint, 1, 2);
                byte[] firstAccepted = EncodeDatagram(
                    new RudpHeader(RudpFlag.Reliable, 1, 2, 3, 1, 1, 0, 23),
                    Array.Empty<byte>());
                await server.SendAsync(
                    firstAccepted, firstAccepted.Length, clientEndpoint);
                Assert.That(await pump.ReceiveOnceAsync(), Is.True);
                Assert.That(pump.TryDequeue(out _), Is.True);

                pump.PrepareForRebind();
                byte[] secondAccepted = EncodeDatagram(
                    new RudpHeader(RudpFlag.Reliable, 1, 2, 4, 1, 1, 0, 23),
                    Array.Empty<byte>());
                await server.SendAsync(
                    secondAccepted, secondAccepted.Length, clientEndpoint);

                Assert.That(await pump.ReceiveOnceAsync(), Is.True);
                Assert.That(pump.TryDequeue(out RudpInboundDatagram rebound), Is.True);
                Assert.That(rebound.Message, Is.TypeOf<RudpBindAccepted>());
                Assert.That(rebound.Header.TransportEpoch, Is.EqualTo(4));

                byte[] previousSnapshot = GoldenBytes(
                    "movement-v1.json", "StateSnapshot", "datagramHex");
                byte[] stale = EncodeDatagram(
                    new RudpHeader(RudpFlag.Unreliable, 1, 2, 3, 99, 1, 0, 26),
                    previousSnapshot.Skip(48).ToArray());
                await server.SendAsync(stale, stale.Length, clientEndpoint);

                Assert.That(await pump.ReceiveOnceAsync(), Is.False);
                Assert.That(pump.TryDequeue(out _), Is.False);
            }
        }

        [Test]
        public async Task BindCapabilityHandoffKeepsOneLatestValue()
        {
            var tcp = new RecordingTcpSender();
            var udp = new RecordingRudpSender();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(udp, pump);
                var client = new BattleMovementClient(
                    tcp, reliable, pump, 1, 2, 7);
                RudpBindCapability capability = RudpProtocolCodec.DecodeBindCapability(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindCapability", "frameHex"));

                await client.RequestBindCapabilityAsync(1, CancellationToken.None);
                client.OnRudpBindCapability(capability);
                client.OnRudpBindCapability(capability);

                Assert.That(
                    await client.DrainBindCapabilitiesAsync(CancellationToken.None),
                    Is.EqualTo(1));
                Assert.That(udp.Datagrams, Has.Count.EqualTo(1));
            }
        }

        [Test]
        public void ReadModelAppliesOnlyTheLatestServerSnapshot()
        {
            var readModel = new BattleMovementReadModel(7);

            Assert.That(readModel.Apply(Snapshot(11, 20, 1, 1000, -2000)), Is.True);
            Assert.That(readModel.Apply(Snapshot(10, 19, 1, 9999, 9999)), Is.False);
            Assert.That(readModel.Apply(Snapshot(11, 21, 1, 9999, 9999)), Is.False);
            Assert.That(readModel.Apply(Snapshot(12, 22, 1, 1250, -1750)), Is.True);
            Assert.That(readModel.SnapshotSequence, Is.EqualTo(12));
            Assert.That(readModel.ServerTick, Is.EqualTo(22));
            Assert.That(readModel.Positions[1].PositionXMillimeters, Is.EqualTo(1250));
            Assert.That(readModel.Positions[1].PositionYMillimeters, Is.EqualTo(-1750));

            var wrap = new BattleMovementReadModel(7);
            Assert.That(wrap.Apply(Snapshot(uint.MaxValue, 1, 1, 1, 1)), Is.True);
            Assert.That(wrap.Apply(Snapshot(1, 2, 1, 2, 2)), Is.True);
        }

        [Test]
        public async Task BindingHeartbeatAndMoveUseCanonicalOutboundChannel()
        {
            var tcp = new RecordingTcpSender();
            var udp = new RecordingRudpSender();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var client = new BattleMovementClient(tcp, udp, pump, 1, 2, 7);

                await client.RequestBindCapabilityAsync(1, CancellationToken.None);
                await client.AcceptBindCapabilityAsync(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindCapability", "frameHex"),
                    CancellationToken.None);
                client.ApplyInbound(
                    RudpProtocolCodec.DecodeInbound(
                        GoldenBytes("rudp-bind-v1.json", "RudpBindAccepted", "datagramHex")));
                await client.TickAsync(100, CancellationToken.None);
                await client.TickAsync(1099, CancellationToken.None);
                await client.SendDirectionAsync(
                    short.MaxValue, -short.MaxValue, CancellationToken.None);

                Assert.That(tcp.Frames, Has.Count.EqualTo(1));
                CollectionAssert.AreEqual(
                    GoldenBytes("rudp-bind-v1.json", "RequestRudpBindCapability", "frameHex"),
                    tcp.Frames[0]);
                Assert.That(udp.Datagrams, Has.Count.EqualTo(3));
                CollectionAssert.AreEqual(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindHello", "datagramHex"),
                    udp.Datagrams[0]);
                CollectionAssert.AreEqual(
                    GoldenBytes("rudp-bind-v1.json", "RudpHeartbeat", "datagramHex"),
                    udp.Datagrams[1]);
                Assert.That(ReadUInt16(udp.Datagrams[2], 40), Is.EqualTo(25));
                Assert.That(client.IsBound, Is.True);
                Assert.That(client.TransportEpoch, Is.EqualTo(3));
            }
        }

        [Test]
        public async Task ReliableOwnerRecoversDroppedBindPacketsAndRetiresTheSameDatagram()
        {
            var tcp = new RecordingTcpSender();
            var udp = new RecordingRudpSender();
            long now = 0;
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(udp, pump, () => now);
                var client = new BattleMovementClient(
                    tcp, reliable, pump, 1, 2, 7);

                await client.RequestBindCapabilityAsync(1, CancellationToken.None);
                await client.AcceptBindCapabilityAsync(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindCapability", "frameHex"),
                    CancellationToken.None);
                Assert.That(udp.Datagrams, Has.Count.EqualTo(1));

                now = 199;
                await reliable.TickAsync(CancellationToken.None);
                Assert.That(udp.Datagrams, Has.Count.EqualTo(1));
                now = 200;
                await reliable.TickAsync(CancellationToken.None);
                Assert.That(udp.Datagrams, Has.Count.EqualTo(2));
                CollectionAssert.AreEqual(udp.Datagrams[0], udp.Datagrams[1]);

                Assert.That(client.ApplyInbound(RudpProtocolCodec.DecodeInbound(
                    GoldenBytes(
                        "rudp-bind-v1.json", "RudpBindAccepted", "datagramHex"))),
                    Is.True);
                now = 600;
                await reliable.TickAsync(CancellationToken.None);

                Assert.That(udp.Datagrams, Has.Count.EqualTo(2));
                Assert.That(reliable.PendingReliableCount, Is.Zero);
                Assert.That(reliable.HasConfirmedFailure, Is.False);
                Assert.That(client.TransportEpoch, Is.EqualTo(3));
            }
        }

        [Test]
        public async Task ReliableOwnerRecoversWhenTheFirstBindAcceptedIsDropped()
        {
            var tcp = new RecordingTcpSender();
            var udp = new RecordingRudpSender();
            long now = 0;
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(udp, pump, () => now);
                var client = new BattleMovementClient(
                    tcp, reliable, pump, 1, 2, 7);

                await client.RequestBindCapabilityAsync(1, CancellationToken.None);
                await client.AcceptBindCapabilityAsync(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindCapability", "frameHex"),
                    CancellationToken.None);
                now = 200;
                await reliable.TickAsync(CancellationToken.None);

                CollectionAssert.AreEqual(udp.Datagrams[0], udp.Datagrams[1]);
                Assert.That(client.IsBound, Is.False);
                Assert.That(client.ApplyInbound(RudpProtocolCodec.DecodeInbound(
                    GoldenBytes(
                        "rudp-bind-v1.json", "RudpBindAccepted", "datagramHex"))),
                    Is.True);
                Assert.That(client.TransportEpoch, Is.EqualTo(3));
                Assert.That(reliable.PendingReliableCount, Is.Zero);
            }
        }

        [Test]
        public async Task PreparedRebindUsesTheSameSessionReliableOwner()
        {
            var tcp = new RecordingTcpSender();
            var udp = new RecordingRudpSender();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(udp, pump, () => 0);
                var client = new BattleMovementClient(
                    tcp, reliable, pump, 1, 2, 7);

                await client.RequestBindCapabilityAsync(1, CancellationToken.None);
                await client.AcceptBindCapabilityAsync(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindCapability", "frameHex"),
                    CancellationToken.None);
                Assert.That(client.ApplyInbound(RudpProtocolCodec.DecodeInbound(
                    GoldenBytes(
                        "rudp-bind-v1.json", "RudpBindAccepted", "datagramHex"))),
                    Is.True);
                Assert.That(client.IsBound, Is.True);

                await client.RequestBindCapabilityAsync(2, CancellationToken.None);
                Assert.That(client.IsBound, Is.False);
                byte[] capability = GoldenBytes(
                    "rudp-bind-v1.json", "RudpBindCapability", "frameHex");
                WriteUInt64(capability, 9, 2);
                await client.AcceptBindCapabilityAsync(
                    capability, CancellationToken.None);
                byte[] rebound = EncodeDatagram(
                    new RudpHeader(RudpFlag.Reliable, 1, 2, 4, 1, 2, 0, 23),
                    Array.Empty<byte>());

                Assert.That(client.ApplyInbound(
                    RudpProtocolCodec.DecodeInbound(rebound)), Is.True);
                Assert.That(client.TransportEpoch, Is.EqualTo(4));
                Assert.That(reliable.PendingReliableCount, Is.Zero);
            }
        }

        [Test]
        public async Task ReliableOwnerStopsAfterFiveTransmissionsAndFailsAtFiveSeconds()
        {
            var tcp = new RecordingTcpSender();
            var udp = new RecordingRudpSender();
            long now = 0;
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(udp, pump, () => now);
                var client = new BattleMovementClient(
                    tcp, reliable, pump, 1, 2, 7);

                await client.RequestBindCapabilityAsync(1, CancellationToken.None);
                await client.AcceptBindCapabilityAsync(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindCapability", "frameHex"),
                    CancellationToken.None);
                foreach (long retryAt in new long[] { 200, 600, 1400, 2400 })
                {
                    now = retryAt;
                    await reliable.TickAsync(CancellationToken.None);
                }
                Assert.That(udp.Datagrams, Has.Count.EqualTo(5));
                Assert.That(
                    udp.Datagrams.All(datagram =>
                        datagram.SequenceEqual(udp.Datagrams[0])),
                    Is.True);

                now = 4999;
                await reliable.TickAsync(CancellationToken.None);
                Assert.That(reliable.HasConfirmedFailure, Is.False);
                now = 5000;
                await reliable.TickAsync(CancellationToken.None);

                Assert.That(udp.Datagrams, Has.Count.EqualTo(5));
                Assert.That(reliable.PendingReliableCount, Is.Zero);
                Assert.That(reliable.HasConfirmedFailure, Is.True);
            }
        }

        [Test]
        public async Task ReliableOwnerKeepsTheFrozenQueueBounds()
        {
            Assert.That(Constant("TotalCapacity"), Is.EqualTo(256));
            Assert.That(Constant("ApplicationCapacity"), Is.EqualTo(224));
            Assert.That(Constant("ByteCapacity"), Is.EqualTo(256 * 1024));

            var udp = new RecordingRudpSender();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(udp, pump, () => 0);
                byte[] capability = RudpProtocolCodec.DecodeBindCapability(
                    GoldenBytes(
                        "rudp-bind-v1.json", "RudpBindCapability", "frameHex"))
                    .Bytes;

                for (int index = 0; index < 256; index++)
                {
                    await reliable.SendBindHelloAsync(
                        capability, CancellationToken.None);
                }
                Assert.That(reliable.PendingReliableCount, Is.EqualTo(256));
                Assert.ThrowsAsync<InvalidOperationException>(async () =>
                    await reliable.SendBindHelloAsync(
                        capability, CancellationToken.None));
                Assert.That(reliable.HasConfirmedFailure, Is.True);
            }
        }

        [Test]
        public async Task BoundClientBuildsTheAttackIntentHeader()
        {
            var tcp = new RecordingTcpSender();
            var udp = new RecordingRudpSender();
            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(udp, pump);
                var client = new BattleMovementClient(
                    tcp, reliable, pump, 1, 2, 7);
                await client.RequestBindCapabilityAsync(1, CancellationToken.None);
                await client.AcceptBindCapabilityAsync(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindCapability", "frameHex"),
                    CancellationToken.None);
                client.ApplyInbound(RudpProtocolCodec.DecodeInbound(
                    GoldenBytes("rudp-bind-v1.json", "RudpBindAccepted", "datagramHex")));

                await reliable.SendAttackAsync(7, 1, CancellationToken.None);
                byte[] datagram = udp.Datagrams.Last();

                Assert.That(ReadUInt16(datagram, 40), Is.EqualTo(27));
            }
        }

        [Test]
        public async Task ExistingTcpPumpRoutesBindCapabilityWithoutASecondReceiveOwner()
        {
            byte[] capability = GoldenBytes(
                "rudp-bind-v1.json", "RudpBindCapability", "frameHex");
            byte[] entry = GoldenBytes(
                "battle-load-v1.json", "ArenaLoadEntry", "frameHex");
            var stream = new byte[capability.Length + entry.Length];
            Buffer.BlockCopy(capability, 0, stream, 0, capability.Length);
            Buffer.BlockCopy(entry, 0, stream, capability.Length, entry.Length);
            var battleSink = new RecordingBattleSink();
            var capabilitySink = new RecordingCapabilitySink();
            var router = new TypedServerEventRouter(new ImmediateMainThreadDispatcher());

            using (router.Subscribe((ITcpInboundMessageSink)battleSink))
            using (router.Subscribe((IRudpBindCapabilitySink)capabilitySink))
            {
                await new TcpInboundPump(new MemoryStream(stream), router)
                    .RunAsync(CancellationToken.None);
            }

            Assert.That(capabilitySink.Capabilities, Has.Count.EqualTo(1));
            Assert.That(capabilitySink.Capabilities[0].RequestId, Is.EqualTo(1));
            Assert.That(battleSink.Messages, Has.Count.EqualTo(1));
        }

        private static RudpStateSnapshot Snapshot(
            uint sequence,
            uint tick,
            ulong sessionId,
            int x,
            int y)
        {
            return new RudpStateSnapshot(
                7,
                sequence,
                tick,
                new[] { new RudpSnapshotPlayer(sessionId, x, y) });
        }

        private static ushort ReadUInt16(byte[] bytes, int offset)
        {
            return (ushort)((bytes[offset] << 8) | bytes[offset + 1]);
        }

        private static void WriteUInt64(byte[] bytes, int offset, ulong value)
        {
            for (int index = 7; index >= 0; index--)
            {
                bytes[offset + index] = (byte)value;
                value >>= 8;
            }
        }

        private static int Constant(string name)
        {
            FieldInfo field = typeof(RudpReliableOutbound).GetField(
                name,
                BindingFlags.Static | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null);
            return (int)field.GetRawConstantValue();
        }

        private static byte[] GoldenBytes(string filename, string semanticName, string field)
        {
            string path = Path.GetFullPath(Path.Combine(
                Application.dataPath,
                "../../../contracts/protocol/golden",
                filename));
            string contract = File.ReadAllText(path);
            string semanticMarker = $"\"semanticName\": \"{semanticName}\"";
            int semanticOffset = contract.IndexOf(semanticMarker, StringComparison.Ordinal);
            Assert.That(semanticOffset, Is.GreaterThanOrEqualTo(0));
            string valueMarker = $"\"{field}\": \"";
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

        private static byte[] EncodeDatagram(RudpHeader header, byte[] payload)
        {
            MethodInfo encode = typeof(RudpProtocolCodec).GetMethod(
                "Encode",
                BindingFlags.Static | BindingFlags.NonPublic);
            Assert.That(encode, Is.Not.Null);
            return (byte[])encode.Invoke(null, new object[] { header, payload });
        }

        private sealed class RecordingTcpSender : ITcpCommandSender
        {
            public List<byte[]> Frames { get; } = new List<byte[]>();

            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                Frames.Add(frame);
                return Task.CompletedTask;
            }
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

        private sealed class RecordingBattleSink : ITcpInboundMessageSink
        {
            public List<LootOfLegends.Protocol.BattleLoadServerMessage> Messages { get; } =
                new List<LootOfLegends.Protocol.BattleLoadServerMessage>();

            public void OnMessage(LootOfLegends.Protocol.BattleLoadServerMessage message)
            {
                Messages.Add(message);
            }
        }

        private sealed class RecordingCapabilitySink : IRudpBindCapabilitySink
        {
            public List<RudpBindCapability> Capabilities { get; } =
                new List<RudpBindCapability>();

            public void OnRudpBindCapability(RudpBindCapability capability)
            {
                Capabilities.Add(capability);
            }
        }
    }
}
