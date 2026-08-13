using System;
using System.Collections.Generic;
using System.Threading;
using LootOfLegends.Protocol;
using LootOfLegends.Transport.Rudp;

namespace LootOfLegends.Transport
{
    public interface IMainThreadDispatcher
    {
        void Post(Action action);
    }

    public sealed class SynchronizationContextMainThreadDispatcher : IMainThreadDispatcher
    {
        private readonly SynchronizationContext context;

        public SynchronizationContextMainThreadDispatcher(SynchronizationContext context)
        {
            this.context = context ?? throw new ArgumentNullException(nameof(context));
        }

        public void Post(Action action)
        {
            if (action == null)
            {
                throw new ArgumentNullException(nameof(action));
            }
            context.Post(_ => action(), null);
        }
    }

    public sealed class TypedServerEventRouter
    {
        private readonly object gate = new object();
        private readonly IMainThreadDispatcher dispatcher;
        private readonly List<RoutedSink<ITcpInboundMessageSink>> battleLoadSinks =
            new List<RoutedSink<ITcpInboundMessageSink>>();
        private readonly List<RoutedSink<ISessionInboundMessageSink>> sessionSinks =
            new List<RoutedSink<ISessionInboundMessageSink>>();
        private readonly List<RoutedSink<ILobbyRoomInboundMessageSink>> lobbyRoomSinks =
            new List<RoutedSink<ILobbyRoomInboundMessageSink>>();
        private readonly List<RoutedSink<IFinalResultInboundMessageSink>> finalResultSinks =
            new List<RoutedSink<IFinalResultInboundMessageSink>>();
        private readonly List<RoutedSink<IBattleRecoveryInboundMessageSink>> recoverySinks =
            new List<RoutedSink<IBattleRecoveryInboundMessageSink>>();
        private readonly List<RoutedSink<IRudpBindCapabilitySink>> rudpBindSinks =
            new List<RoutedSink<IRudpBindCapabilitySink>>();

        public TypedServerEventRouter(IMainThreadDispatcher dispatcher)
        {
            this.dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
        }

        public IDisposable Subscribe(ITcpInboundMessageSink sink)
        {
            return AddUnique(battleLoadSinks, sink, "Battle load");
        }

        public IDisposable Subscribe(ISessionInboundMessageSink sink)
        {
            return AddUnique(sessionSinks, sink, "Session");
        }

        public IDisposable Subscribe(ILobbyRoomInboundMessageSink sink)
        {
            return AddUnique(lobbyRoomSinks, sink, "Lobby/Room");
        }

        public IDisposable Subscribe(IFinalResultInboundMessageSink sink)
        {
            return AddUnique(finalResultSinks, sink, "Final Result");
        }

        public IDisposable Subscribe(IBattleRecoveryInboundMessageSink sink)
        {
            return AddUnique(recoverySinks, sink, "Battle recovery");
        }

        public IDisposable Subscribe(IRudpBindCapabilitySink sink)
        {
            return AddUnique(rudpBindSinks, sink, "RUDP bind");
        }

        public void Dispatch(BattleLoadServerMessage message)
        {
            DispatchSnapshot(Snapshot(battleLoadSinks), message, (sink, value) =>
                sink.OnMessage(value));
        }

        public void Dispatch(SessionServerMessage message)
        {
            DispatchSnapshot(Snapshot(sessionSinks), message, (sink, value) =>
                sink.OnMessage(value));
        }

        public void Dispatch(LobbyRoomServerMessage message)
        {
            DispatchSnapshot(Snapshot(lobbyRoomSinks), message, (sink, value) =>
                sink.OnMessage(value));
        }

        public void Dispatch(BattleFinalResult message)
        {
            DispatchSnapshot(Snapshot(finalResultSinks), message, (sink, value) =>
                sink.OnMessage(value));
        }

        public void Dispatch(BattleRecoveryNotice message)
        {
            DispatchSnapshot(Snapshot(recoverySinks), message, (sink, value) =>
                sink.OnMessage(value));
        }

        public void Dispatch(RudpBindCapability capability)
        {
            DispatchSnapshot(Snapshot(rudpBindSinks), capability, (sink, value) =>
                sink.OnRudpBindCapability(value));
        }

        private IDisposable AddUnique<T>(List<RoutedSink<T>> sinks, T sink, string lane)
            where T : class
        {
            if (sink == null)
            {
                throw new ArgumentNullException(nameof(sink));
            }
            lock (gate)
            {
                if (sinks.Exists(candidate =>
                        ReferenceEquals(candidate.Sink, sink) &&
                        Volatile.Read(ref candidate.Active) != 0))
                {
                    throw new InvalidOperationException($"{lane} sink is already subscribed");
                }
                var routed = new RoutedSink<T>(sink);
                sinks.Add(routed);
                return new Subscription(() =>
                {
                    Volatile.Write(ref routed.Active, 0);
                    lock (gate)
                    {
                        sinks.Remove(routed);
                    }
                });
            }
        }

        private RoutedSink<T>[] Snapshot<T>(ICollection<RoutedSink<T>> sinks)
        {
            lock (gate)
            {
                var snapshot = new RoutedSink<T>[sinks.Count];
                sinks.CopyTo(snapshot, 0);
                return snapshot;
            }
        }

        private void DispatchSnapshot<TSink, TMessage>(
            IEnumerable<RoutedSink<TSink>> sinks,
            TMessage message,
            Action<TSink, TMessage> deliver)
            where TMessage : class
        {
            if (message == null)
            {
                throw new ArgumentNullException(nameof(message));
            }
            foreach (RoutedSink<TSink> sink in sinks)
            {
                RoutedSink<TSink> capturedSink = sink;
                dispatcher.Post(() =>
                {
                    if (Volatile.Read(ref capturedSink.Active) != 0)
                    {
                        deliver(capturedSink.Sink, message);
                    }
                });
            }
        }

        private sealed class RoutedSink<T>
        {
            public RoutedSink(T sink)
            {
                Sink = sink;
                Active = 1;
            }

            public T Sink { get; }
            public int Active;
        }

        private sealed class Subscription : IDisposable
        {
            private Action unsubscribe;

            public Subscription(Action unsubscribe)
            {
                this.unsubscribe = unsubscribe;
            }

            public void Dispose()
            {
                Interlocked.Exchange(ref unsubscribe, null)?.Invoke();
            }
        }
    }
}
