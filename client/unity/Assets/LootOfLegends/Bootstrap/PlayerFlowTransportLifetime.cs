using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Transport;
using LootOfLegends.Transport.Rudp;

namespace LootOfLegends.Bootstrap
{
    public sealed class PlayerFlowTransportLifetime
    {
        private readonly object gate = new object();
        private readonly CancellationTokenSource shutdown;
        private readonly IMainThreadDispatcher dispatcher;
        private readonly Action onRudpConfirmedFailure;
        private Task tcpTask;
        private Task rudpTask;
        private bool failurePosted;
        private bool stopped;

        public PlayerFlowTransportLifetime()
            : this(CancellationToken.None, null, null)
        {
        }

        public PlayerFlowTransportLifetime(
            CancellationToken parentToken,
            IMainThreadDispatcher dispatcher,
            Action onRudpConfirmedFailure)
        {
            if ((dispatcher == null) != (onRudpConfirmedFailure == null))
            {
                throw new ArgumentException(
                    "RUDP failure dispatcher and callback must be configured together");
            }
            shutdown = CancellationTokenSource.CreateLinkedTokenSource(parentToken);
            this.dispatcher = dispatcher;
            this.onRudpConfirmedFailure = onRudpConfirmedFailure;
        }

        public bool IsRunning
        {
            get
            {
                lock (gate)
                {
                    return !stopped && !shutdown.IsCancellationRequested &&
                           (tcpTask != null || rudpTask != null);
                }
            }
        }

        public Task TcpTask
        {
            get
            {
                lock (gate)
                {
                    return tcpTask;
                }
            }
        }

        public Task RudpTask
        {
            get
            {
                lock (gate)
                {
                    return rudpTask;
                }
            }
        }

        public Task StartTcp(TcpInboundPump pump)
        {
            if (pump == null)
            {
                throw new ArgumentNullException(nameof(pump));
            }
            lock (gate)
            {
                ThrowIfStopped();
                if (tcpTask != null)
                {
                    throw new InvalidOperationException(
                        "TCP inbound pump can start only once");
                }
                tcpTask = pump.RunAsync(shutdown.Token);
                return tcpTask;
            }
        }

        public Task StartRudp(RudpInboundPump pump)
        {
            if (pump == null)
            {
                throw new ArgumentNullException(nameof(pump));
            }
            lock (gate)
            {
                ThrowIfStopped();
                if (rudpTask != null)
                {
                    throw new InvalidOperationException(
                        "RUDP inbound pump can start only once");
                }
                rudpTask = RunRudpAsync(pump, shutdown.Token);
                return rudpTask;
            }
        }

        public void ConfirmRudpFailure()
        {
            Action callback = null;
            lock (gate)
            {
                if (failurePosted || stopped)
                {
                    return;
                }
                failurePosted = true;
                callback = onRudpConfirmedFailure;
                shutdown.Cancel();
            }
            if (callback != null)
            {
                dispatcher.Post(callback);
            }
        }

        public async Task StopAsync()
        {
            Task tcp;
            Task rudp;
            lock (gate)
            {
                if (stopped)
                {
                    return;
                }
                stopped = true;
                shutdown.Cancel();
                tcp = tcpTask ?? Task.CompletedTask;
                rudp = rudpTask ?? Task.CompletedTask;
            }
            try
            {
                await Task.WhenAll(tcp, rudp).ConfigureAwait(false);
            }
            finally
            {
                shutdown.Dispose();
            }
        }

        private async Task RunRudpAsync(
            RudpInboundPump pump,
            CancellationToken cancellationToken)
        {
            try
            {
                await pump.RunAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (Exception) when (!cancellationToken.IsCancellationRequested)
            {
                ConfirmRudpFailure();
            }
        }

        private void ThrowIfStopped()
        {
            if (stopped || shutdown.IsCancellationRequested)
            {
                throw new InvalidOperationException(
                    "Player-flow transport lifetime is stopped");
            }
        }
    }
}
