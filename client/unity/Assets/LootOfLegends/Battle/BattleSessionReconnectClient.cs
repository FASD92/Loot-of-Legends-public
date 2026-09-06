using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;

namespace LootOfLegends.Battle
{
    public sealed class BattleRecoveryResumeProof
    {
        public BattleRecoveryResumeProof(
            ulong roomId,
            ulong battleInstanceId,
            ulong sessionId,
            ulong sessionGeneration)
        {
            if (roomId == 0 || battleInstanceId == 0 || sessionId == 0 ||
                sessionGeneration == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(roomId));
            }
            RoomId = roomId;
            BattleInstanceId = battleInstanceId;
            SessionId = sessionId;
            SessionGeneration = sessionGeneration;
        }

        public ulong RoomId { get; }
        public ulong BattleInstanceId { get; }
        public ulong SessionId { get; }
        public ulong SessionGeneration { get; }
    }

    public enum BattleSessionReconnectState
    {
        Idle,
        Reconnecting,
        ApplyingSnapshot,
        Resumed,
        GraceExpired,
        AuthenticationFailed,
        Failed
    }

    public enum BattleSessionReconnectStatus
    {
        Resumed,
        AlreadyRunning,
        GraceExpired,
        AuthenticationFailed,
        Failed,
        Cancelled
    }

    public sealed class BattleSessionReconnectResult
    {
        internal BattleSessionReconnectResult(
            BattleSessionReconnectStatus status,
            BattleResumeSnapshot snapshot = null,
            Exception error = null)
        {
            Status = status;
            Snapshot = snapshot;
            Error = error;
        }

        public BattleSessionReconnectStatus Status { get; }
        public BattleResumeSnapshot Snapshot { get; }
        public Exception Error { get; }
        public bool RequiresRudpRebind =>
            Snapshot != null && Snapshot.Phase != BattleResumePhase.Result;
    }

    public sealed class BattleResumeUnavailableException : Exception
    {
        public BattleResumeUnavailableException()
            : base("Battle resume is unavailable")
        {
        }
    }

    public sealed class BattleResumeRejectedException : Exception
    {
        public BattleResumeRejectedException(AuthenticationRejectedReason reason)
            : base("Battle resume authentication was rejected")
        {
            Reason = reason;
        }

        public AuthenticationRejectedReason Reason { get; }
    }

    public interface IMonotonicReconnectClock
    {
        long NowMilliseconds { get; }
        Task DelayAsync(int milliseconds, CancellationToken cancellationToken);
    }

    public sealed class StopwatchReconnectClock : IMonotonicReconnectClock
    {
        public long NowMilliseconds =>
            (long)(Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency);

        public Task DelayAsync(int milliseconds, CancellationToken cancellationToken)
        {
            return Task.Delay(milliseconds, cancellationToken);
        }
    }

    public sealed class BattleSessionReconnectClient :
        IBattleResumeInboundMessageSink,
        ISessionInboundMessageSink
    {
        private const long GracePeriodMilliseconds = 30000;
        private static readonly int[] BackoffMilliseconds =
            { 0, 250, 500, 1000, 2000, 4000, 5000 };

        private readonly ITcpCommandSender initialSender;
        private readonly Func<CancellationToken, Task<string>> issueCredential;
        private readonly IMonotonicReconnectClock clock;
        private readonly Func<BattleResumeSnapshot, bool> applySnapshot;
        private readonly IMainThreadDispatcher dispatcher;
        private readonly Func<ulong, WelcomeSession, bool> applyResumedWelcome;
        private readonly object gate = new object();
        private TaskCompletionSource<WelcomeSession> pendingWelcome;
        private TaskCompletionSource<BattleResumeSnapshot> pendingSnapshot;
        private ulong pendingRequestId;
        private int running;
        private long nextRequestId;

        public BattleSessionReconnectClient(
            ITcpCommandSender initialSender,
            Func<CancellationToken, Task<string>> issueCredential,
            IMonotonicReconnectClock clock,
            Func<BattleResumeSnapshot, bool> applySnapshot,
            IMainThreadDispatcher dispatcher = null,
            Func<ulong, WelcomeSession, bool> applyResumedWelcome = null)
        {
            this.initialSender = initialSender ??
                throw new ArgumentNullException(nameof(initialSender));
            this.issueCredential = issueCredential ??
                throw new ArgumentNullException(nameof(issueCredential));
            this.clock = clock ?? throw new ArgumentNullException(nameof(clock));
            this.applySnapshot = applySnapshot ??
                throw new ArgumentNullException(nameof(applySnapshot));
            this.dispatcher = dispatcher;
            this.applyResumedWelcome = applyResumedWelcome ??
                ((_, __) => true);
        }

        public event Action Changed;

        public BattleSessionReconnectState State { get; private set; }
        public bool IsInputLocked { get; private set; }
        public ulong CurrentSessionGeneration { get; private set; }

        public async Task<BattleSessionReconnectResult> ResumeAsync(
            ulong previousSessionId,
            ulong previousSessionGeneration,
            Func<CancellationToken, Task<ITcpCommandSender>> connect,
            CancellationToken cancellationToken,
            BattleRecoveryResumeProof recoveryProof = null)
        {
            if (previousSessionId == 0 || previousSessionGeneration == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(previousSessionId));
            }
            if (connect == null)
            {
                throw new ArgumentNullException(nameof(connect));
            }
            if (recoveryProof != null &&
                recoveryProof.SessionId != previousSessionId)
            {
                throw new ArgumentException(
                    "Battle recovery proof belongs to another session",
                    nameof(recoveryProof));
            }
            if (Interlocked.CompareExchange(ref running, 1, 0) != 0)
            {
                return new BattleSessionReconnectResult(
                    BattleSessionReconnectStatus.AlreadyRunning);
            }

            SetState(BattleSessionReconnectState.Reconnecting, true);
            CurrentSessionGeneration = previousSessionGeneration;
            ulong originalProofGeneration = recoveryProof == null
                ? previousSessionGeneration
                : recoveryProof.SessionGeneration;
            ulong currentGeneration = previousSessionGeneration;
            bool originalProofRetried = false;
            long startedAt = clock.NowMilliseconds;
            int attempt = 0;
            try
            {
                while (true)
                {
                    int backoff = BackoffMilliseconds[
                        Math.Min(attempt, BackoffMilliseconds.Length - 1)];
                    if (attempt > 0)
                    {
                        await clock.DelayAsync(backoff, cancellationToken)
                            .ConfigureAwait(false);
                    }
                    if (clock.NowMilliseconds - startedAt >= GracePeriodMilliseconds)
                    {
                        return Finish(
                            BattleSessionReconnectStatus.GraceExpired,
                            BattleSessionReconnectState.GraceExpired);
                    }

                    cancellationToken.ThrowIfCancellationRequested();
                    string credential;
                    try
                    {
                        credential = await issueCredential(cancellationToken)
                            .ConfigureAwait(false);
                        if (string.IsNullOrEmpty(credential))
                        {
                            throw new BattleResumeUnavailableException();
                        }
                    }
                    catch (BattleResumeRejectedException)
                    {
                        return Finish(
                            BattleSessionReconnectStatus.AuthenticationFailed,
                            BattleSessionReconnectState.AuthenticationFailed);
                    }
                    catch (OperationCanceledException) when (
                        cancellationToken.IsCancellationRequested)
                    {
                        return Finish(
                            BattleSessionReconnectStatus.Cancelled,
                            BattleSessionReconnectState.Failed);
                    }
                    catch (Exception error)
                    {
                        if (clock.NowMilliseconds - startedAt >= GracePeriodMilliseconds)
                        {
                            return Finish(
                                BattleSessionReconnectStatus.GraceExpired,
                                BattleSessionReconnectState.GraceExpired,
                                error: error);
                        }
                        attempt++;
                        continue;
                    }

                    ulong requestId = NextRequestId();
                    ITcpCommandSender sender;
                    try
                    {
                        sender = await connect(cancellationToken).ConfigureAwait(false) ??
                            initialSender;
                        BeginResumeWait(requestId);
                        await sender.SendAsync(
                                BattleSessionResumeProtocolCodec.EncodeResumeBattleSession(
                                    requestId,
                                    previousSessionId,
                                    currentGeneration,
                                    credential),
                                cancellationToken)
                            .ConfigureAwait(false);
                        WelcomeSession welcome = await WaitForWelcomeAsync(
                                requestId,
                                cancellationToken)
                            .ConfigureAwait(false);
                        if (welcome.SessionId != previousSessionId ||
                            welcome.SessionGeneration < currentGeneration)
                        {
                            return Finish(
                                BattleSessionReconnectStatus.Failed,
                                BattleSessionReconnectState.Failed);
                        }
                        if (!await ApplyResumedWelcomeAsync(requestId, welcome)
                                .ConfigureAwait(false))
                        {
                            return Finish(
                                BattleSessionReconnectStatus.Failed,
                                BattleSessionReconnectState.Failed);
                        }
                        currentGeneration = welcome.SessionGeneration;
                        CurrentSessionGeneration = currentGeneration;
                        BattleResumeSnapshot snapshot = await WaitForSnapshotAsync(
                                requestId,
                                cancellationToken)
                            .ConfigureAwait(false);
                        if (snapshot.RoomId == 0 ||
                            (recoveryProof != null &&
                             (snapshot.RoomId != recoveryProof.RoomId ||
                              snapshot.BattleInstanceId != recoveryProof.BattleInstanceId)) ||
                            snapshot.PlayerSessionId != previousSessionId ||
                            snapshot.SessionGeneration != welcome.SessionGeneration)
                        {
                            throw new BattleResumeUnavailableException();
                        }
                        SetState(BattleSessionReconnectState.ApplyingSnapshot, true);
                        if (!await ApplySnapshotAsync(snapshot).ConfigureAwait(false))
                        {
                            return Finish(
                                BattleSessionReconnectStatus.Failed,
                                BattleSessionReconnectState.Failed);
                        }
                        await sender.SendAsync(
                                BattleSessionResumeProtocolCodec.EncodeSnapshotApplied(
                                    snapshot.SnapshotId),
                                cancellationToken)
                            .ConfigureAwait(false);
                        return Finish(
                            BattleSessionReconnectStatus.Resumed,
                            BattleSessionReconnectState.Resumed,
                            snapshot);
                    }
                    catch (BattleResumeRejectedException error)
                    {
                        ClearSnapshotWait();
                        if (error.Reason == AuthenticationRejectedReason.ResumeUnavailable &&
                            !originalProofRetried &&
                            currentGeneration != originalProofGeneration)
                        {
                            currentGeneration = originalProofGeneration;
                            CurrentSessionGeneration = currentGeneration;
                            originalProofRetried = true;
                            attempt++;
                            continue;
                        }
                        return Finish(
                            BattleSessionReconnectStatus.AuthenticationFailed,
                            BattleSessionReconnectState.AuthenticationFailed,
                            error: error);
                    }
                    catch (OperationCanceledException) when (
                        cancellationToken.IsCancellationRequested)
                    {
                        return Finish(
                            BattleSessionReconnectStatus.Cancelled,
                            BattleSessionReconnectState.Failed);
                    }
                    catch (Exception error)
                    {
                        ClearSnapshotWait();
                        if (clock.NowMilliseconds - startedAt >= GracePeriodMilliseconds)
                        {
                            return Finish(
                                BattleSessionReconnectStatus.GraceExpired,
                                BattleSessionReconnectState.GraceExpired,
                                error: error);
                        }
                        attempt++;
                    }
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return Finish(
                    BattleSessionReconnectStatus.Cancelled,
                    BattleSessionReconnectState.Failed);
            }
            finally
            {
                ClearSnapshotWait();
                Interlocked.Exchange(ref running, 0);
                if (State == BattleSessionReconnectState.Reconnecting ||
                    State == BattleSessionReconnectState.ApplyingSnapshot)
                {
                    SetState(BattleSessionReconnectState.Failed, false);
                }
            }
        }

        public void OnMessage(BattleResumeSnapshot message)
        {
            if (message == null)
            {
                throw new ArgumentNullException(nameof(message));
            }
            lock (gate)
            {
                if (pendingSnapshot != null && message.RequestId == pendingRequestId)
                {
                    pendingSnapshot.TrySetResult(message);
                }
            }
        }

        public void OnMessage(WelcomeSession message)
        {
            if (message == null)
            {
                throw new ArgumentNullException(nameof(message));
            }
            lock (gate)
            {
                if (pendingWelcome != null && message.RequestId == pendingRequestId)
                {
                    pendingWelcome.TrySetResult(message);
                }
            }
        }

        public void OnMessage(SessionServerMessage message)
        {
            if (message is WelcomeSession welcome)
            {
                OnMessage(welcome);
                return;
            }
            if (message is AuthenticationRejectedSession rejected)
            {
                lock (gate)
                {
                    if (pendingRequestId == rejected.RequestId)
                    {
                        var error = new BattleResumeRejectedException(rejected.Reason);
                        pendingWelcome?.TrySetException(error);
                        pendingSnapshot?.TrySetException(error);
                    }
                }
            }
        }

        private BattleSessionReconnectResult Finish(
            BattleSessionReconnectStatus status,
            BattleSessionReconnectState state,
            BattleResumeSnapshot snapshot = null,
            Exception error = null)
        {
            SetState(state, false);
            return new BattleSessionReconnectResult(status, snapshot, error);
        }

        private void SetState(BattleSessionReconnectState state, bool inputLocked)
        {
            State = state;
            IsInputLocked = inputLocked;
            Changed?.Invoke();
        }

        private Task<bool> ApplyResumedWelcomeAsync(
            ulong requestId,
            WelcomeSession welcome)
        {
            if (dispatcher == null)
            {
                return Task.FromResult(applyResumedWelcome(requestId, welcome));
            }
            var completion = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            dispatcher.Post(() =>
            {
                try
                {
                    completion.TrySetResult(applyResumedWelcome(requestId, welcome));
                }
                catch (Exception error)
                {
                    completion.TrySetException(error);
                }
            });
            return completion.Task;
        }

        private Task<bool> ApplySnapshotAsync(BattleResumeSnapshot snapshot)
        {
            if (dispatcher == null)
            {
                return Task.FromResult(applySnapshot(snapshot));
            }
            var completion = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            dispatcher.Post(() =>
            {
                try
                {
                    completion.TrySetResult(applySnapshot(snapshot));
                }
                catch (Exception error)
                {
                    completion.TrySetException(error);
                }
            });
            return completion.Task;
        }

        private ulong NextRequestId()
        {
            long value = Interlocked.Increment(ref nextRequestId);
            if (value <= 0)
            {
                throw new InvalidOperationException("Battle resume request id space exhausted");
            }
            return (ulong)value;
        }

        private void BeginResumeWait(ulong requestId)
        {
            lock (gate)
            {
                pendingRequestId = requestId;
                pendingWelcome = new TaskCompletionSource<WelcomeSession>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                pendingSnapshot = new TaskCompletionSource<BattleResumeSnapshot>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
            }
        }

        private async Task<WelcomeSession> WaitForWelcomeAsync(
            ulong requestId,
            CancellationToken cancellationToken)
        {
            Task<WelcomeSession> welcome;
            lock (gate)
            {
                if (pendingWelcome == null || pendingRequestId != requestId)
                {
                    throw new InvalidOperationException("Battle resume welcome wait is stale");
                }
                welcome = pendingWelcome.Task;
            }
            using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(
                       cancellationToken))
            {
                timeout.CancelAfter((int)GracePeriodMilliseconds);
                Task completed = await Task.WhenAny(
                        welcome,
                        Task.Delay(Timeout.Infinite, timeout.Token))
                    .ConfigureAwait(false);
                if (completed != welcome)
                {
                    throw new BattleResumeUnavailableException();
                }
                return await welcome.ConfigureAwait(false);
            }
        }

        private async Task<BattleResumeSnapshot> WaitForSnapshotAsync(
            ulong requestId,
            CancellationToken cancellationToken)
        {
            Task<BattleResumeSnapshot> snapshot;
            lock (gate)
            {
                if (pendingSnapshot == null || pendingRequestId != requestId)
                {
                    throw new InvalidOperationException("Battle resume snapshot wait is stale");
                }
                snapshot = pendingSnapshot.Task;
            }
            using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(
                       cancellationToken))
            {
                timeout.CancelAfter((int)GracePeriodMilliseconds);
                Task completed = await Task.WhenAny(
                        snapshot,
                        Task.Delay(Timeout.Infinite, timeout.Token))
                    .ConfigureAwait(false);
                if (completed != snapshot)
                {
                    throw new BattleResumeUnavailableException();
                }
                return await snapshot.ConfigureAwait(false);
            }
        }

        private void ClearSnapshotWait()
        {
            lock (gate)
            {
                pendingRequestId = 0;
                pendingWelcome = null;
                pendingSnapshot = null;
            }
        }
    }
}
