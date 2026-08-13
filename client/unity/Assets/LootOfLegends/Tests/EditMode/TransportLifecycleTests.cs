using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Bootstrap;
using LootOfLegends.Protocol;
using LootOfLegends.Session;
using LootOfLegends.Transport;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class TransportLifecycleTests
    {
        [Test]
        public async Task TcpAndRudpAllowExactlyOnePumpAndStopOnCancellation()
        {
            var dispatcher = new ManualMainThreadDispatcher();
            var router = new TypedServerEventRouter(dispatcher);
            var stream = new CancellableReadStream();
            var tcp = new TcpInboundPump(stream, router);
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var cancellation = new CancellationTokenSource())
            {
                var serverEndpoint = (IPEndPoint)server.Client.LocalEndPoint;
                var rudp = new RudpInboundPump(client, serverEndpoint, 1, 2);

                Task tcpRun = tcp.RunAsync(cancellation.Token);
                Task rudpRun = rudp.RunAsync(cancellation.Token);
                await stream.ReadStarted;

                Assert.ThrowsAsync<InvalidOperationException>(async () =>
                    await new TcpInboundPump(stream, router)
                        .RunAsync(CancellationToken.None));
                Assert.ThrowsAsync<InvalidOperationException>(async () =>
                    await new RudpInboundPump(client, serverEndpoint, 1, 2)
                        .RunAsync(CancellationToken.None));

                cancellation.Cancel();
                await Task.WhenAll(tcpRun, rudpRun);
                Assert.That(tcpRun.IsCompletedSuccessfully, Is.True);
                Assert.That(rudpRun.IsCompletedSuccessfully, Is.True);
            }
        }

        [Test]
        public void TypedRouterUsesMainThreadQueueAndSceneReloadDoesNotDuplicateSubscription()
        {
            var dispatcher = new ManualMainThreadDispatcher();
            var router = new TypedServerEventRouter(dispatcher);
            var sink = new RecordingBattleSink();
            IDisposable first = router.Subscribe((ITcpInboundMessageSink)sink);

            Assert.Throws<InvalidOperationException>(() =>
                router.Subscribe((ITcpInboundMessageSink)sink));
            router.Dispatch(new ArenaLoadEntry(7, 9));
            Assert.That(sink.Messages, Is.Empty);
            first.Dispose();
            dispatcher.Drain();
            Assert.That(sink.Messages, Is.Empty);

            using (router.Subscribe((ITcpInboundMessageSink)sink))
            {
                router.Dispatch(new ArenaLoadEntry(7, 11));
                dispatcher.Drain();
            }
            Assert.That(sink.Messages, Has.Count.EqualTo(1));
            Assert.That(((ArenaLoadEntry)sink.Messages[0]).BattleInstanceId, Is.EqualTo(11));

            router.Dispatch(new ArenaLoadEntry(7, 12));
            dispatcher.Drain();
            Assert.That(sink.Messages, Has.Count.EqualTo(1));
        }

        [Test]
        public void OrphanResponseIsCountedWithoutMutatingBattleReadModel()
        {
            var correlator = new BattleResponseCorrelator();
            var readModel = new BattleLoadReadModel();
            var router = new BattleLoadMessageRouter(correlator, readModel);

            router.OnMessage(new BattleCommandResponse(99, BattleLoadResultCode.Ok));

            Assert.That(correlator.OrphanedResponseCount, Is.EqualTo(1));
            Assert.That(readModel.BattleInstanceId, Is.Zero);
            Assert.That(readModel.IsWaiting, Is.False);
        }

        [Test]
        public async Task BootstrapOwnsBothPumpsUntilSessionShutdown()
        {
            var dispatcher = new ManualMainThreadDispatcher();
            var router = new TypedServerEventRouter(dispatcher);
            var stream = new CancellableReadStream();
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var lifetime = new PlayerFlowTransportLifetime();
                lifetime.StartTcp(new TcpInboundPump(stream, router));
                lifetime.StartRudp(new RudpInboundPump(
                    client,
                    (IPEndPoint)server.Client.LocalEndPoint,
                    1,
                    2));
                await stream.ReadStarted;
                Assert.That(lifetime.IsRunning, Is.True);
                Assert.Throws<InvalidOperationException>(() =>
                    lifetime.StartTcp(new TcpInboundPump(stream, router)));
                Assert.Throws<InvalidOperationException>(() =>
                    lifetime.StartRudp(new RudpInboundPump(
                        client,
                        (IPEndPoint)server.Client.LocalEndPoint,
                        1,
                        2)));

                await lifetime.StopAsync();
                Assert.That(lifetime.IsRunning, Is.False);
            }
        }

        [Test]
        public async Task BootstrapReportsUnexpectedRudpFailureOnMainThread()
        {
            var dispatcher = new ManualMainThreadDispatcher();
            var router = new TypedServerEventRouter(dispatcher);
            var stream = new CancellableReadStream();
            var session = new PlayerSessionReadModel();
            session.BeginAuthentication();
            session.Apply(new WelcomeSession(1, 2, 3, 1700000000000, "player-one"));
            using (var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            using (var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var lifetime = new PlayerFlowTransportLifetime(
                    CancellationToken.None,
                    dispatcher,
                    () => session.ConfirmRudpFailure());
                lifetime.StartTcp(new TcpInboundPump(stream, router));
                lifetime.StartRudp(new RudpInboundPump(
                    client,
                    (IPEndPoint)server.Client.LocalEndPoint,
                    2,
                    3));
                await stream.ReadStarted;
                client.Close();
                for (int attempt = 0;
                     attempt < 100 && dispatcher.PendingCount == 0;
                     attempt++)
                {
                    await Task.Delay(10);
                }
                Assert.That(dispatcher.PendingCount, Is.EqualTo(1));
                dispatcher.Drain();

                Assert.That(session.LastFailure,
                    Is.EqualTo(PlayerSessionFailure.RudpUnavailable));
                await lifetime.StopAsync();
            }
        }

        private sealed class ManualMainThreadDispatcher : IMainThreadDispatcher
        {
            private readonly Queue<Action> pending = new Queue<Action>();

            public int PendingCount
            {
                get
                {
                    lock (pending)
                    {
                        return pending.Count;
                    }
                }
            }

            public void Post(Action action)
            {
                lock (pending)
                {
                    pending.Enqueue(action);
                }
            }

            public void Drain()
            {
                while (true)
                {
                    Action action;
                    lock (pending)
                    {
                        if (pending.Count == 0)
                        {
                            return;
                        }
                        action = pending.Dequeue();
                    }
                    action();
                }
            }
        }

        private sealed class RecordingBattleSink : ITcpInboundMessageSink
        {
            public List<BattleLoadServerMessage> Messages { get; } =
                new List<BattleLoadServerMessage>();

            public void OnMessage(BattleLoadServerMessage message)
            {
                Messages.Add(message);
            }
        }

        private sealed class CancellableReadStream : Stream
        {
            private readonly TaskCompletionSource<bool> readStarted =
                new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);

            public Task ReadStarted => readStarted.Task;
            public override bool CanRead => true;
            public override bool CanSeek => false;
            public override bool CanWrite => false;
            public override long Length => throw new NotSupportedException();
            public override long Position
            {
                get => throw new NotSupportedException();
                set => throw new NotSupportedException();
            }

            public override void Flush()
            {
            }

            public override int Read(byte[] buffer, int offset, int count)
            {
                throw new NotSupportedException();
            }

            public override async Task<int> ReadAsync(
                byte[] buffer,
                int offset,
                int count,
                CancellationToken cancellationToken)
            {
                readStarted.TrySetResult(true);
                await Task.Delay(Timeout.Infinite, cancellationToken);
                return 0;
            }

            public override long Seek(long offset, SeekOrigin origin)
            {
                throw new NotSupportedException();
            }

            public override void SetLength(long value)
            {
                throw new NotSupportedException();
            }

            public override void Write(byte[] buffer, int offset, int count)
            {
                throw new NotSupportedException();
            }
        }
    }

    internal sealed class ImmediateMainThreadDispatcher : IMainThreadDispatcher
    {
        public void Post(Action action)
        {
            action();
        }
    }
}
