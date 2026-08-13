using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;

namespace LootOfLegends.Battle
{
    public enum BattleCommandOutcome
    {
        Ok,
        RoomNotFound,
        RoomNotOpen,
        NotInRoom,
        NotHost,
        NotEnoughPlayers,
        NotAllReady,
        StaleSession,
        Overloaded,
        Rejected
    }

    public sealed class BattleCommandHandle
    {
        internal BattleCommandHandle(ulong requestId, Task<BattleLoadResultCode> completion)
        {
            RequestId = requestId;
            Completion = completion;
        }

        public ulong RequestId { get; }
        public Task<BattleLoadResultCode> Completion { get; }
    }

    public sealed class BattleResponseCorrelator
    {
        private readonly ConcurrentDictionary<ulong, TaskCompletionSource<BattleLoadResultCode>>
            pending = new ConcurrentDictionary<ulong, TaskCompletionSource<BattleLoadResultCode>>();
        private long orphanedResponseCount;

        public long OrphanedResponseCount => Interlocked.Read(ref orphanedResponseCount);

        public BattleCommandHandle Register(ulong requestId)
        {
            var completion = new TaskCompletionSource<BattleLoadResultCode>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            if (requestId == 0 || !pending.TryAdd(requestId, completion))
            {
                throw new ArgumentException("Battle request correlation must be unique and non-zero", nameof(requestId));
            }
            return new BattleCommandHandle(requestId, completion.Task);
        }

        public bool TryComplete(ulong requestId, BattleLoadResultCode result)
        {
            if (pending.TryRemove(
                    requestId,
                    out TaskCompletionSource<BattleLoadResultCode> completion))
            {
                return completion.TrySetResult(result);
            }
            Interlocked.Increment(ref orphanedResponseCount);
            return false;
        }

        public void Fail(ulong requestId, Exception error)
        {
            if (pending.TryRemove(requestId, out TaskCompletionSource<BattleLoadResultCode> completion))
            {
                completion.TrySetException(error);
            }
        }
    }

    public sealed class BattleLoadCoordinator
    {
        private readonly ITcpCommandSender sender;
        private readonly BattleResponseCorrelator correlator;
        private long nextRequestId;

        public BattleLoadCoordinator(
            ITcpCommandSender sender,
            BattleResponseCorrelator correlator)
        {
            this.sender = sender ?? throw new ArgumentNullException(nameof(sender));
            this.correlator = correlator ?? throw new ArgumentNullException(nameof(correlator));
        }

        public async Task<BattleCommandHandle> HostStartAsync(
            CancellationToken cancellationToken)
        {
            ulong requestId = NextRequestId();
            BattleCommandHandle handle = correlator.Register(requestId);
            await SendAsync(handle, BattleLoadProtocolCodec.EncodeHostStart(requestId), cancellationToken)
                .ConfigureAwait(false);
            return handle;
        }

        public async Task<BattleCommandOutcome> HostStartOutcomeAsync(
            CancellationToken cancellationToken)
        {
            BattleCommandHandle handle = await HostStartAsync(cancellationToken)
                .ConfigureAwait(false);
            BattleLoadResultCode result = await handle.Completion.ConfigureAwait(false);
            return Map(result);
        }

        public async Task<BattleCommandOutcome> CompleteArenaLoadOutcomeAsync(
            ulong roomId,
            ulong battleInstanceId,
            CancellationToken cancellationToken)
        {
            BattleCommandHandle handle = await CompleteArenaLoadAsync(
                    roomId,
                    battleInstanceId,
                    cancellationToken)
                .ConfigureAwait(false);
            BattleLoadResultCode result = await handle.Completion.ConfigureAwait(false);
            return Map(result);
        }

        private static BattleCommandOutcome Map(BattleLoadResultCode result)
        {
            switch (result)
            {
                case BattleLoadResultCode.Ok:
                    return BattleCommandOutcome.Ok;
                case BattleLoadResultCode.RoomNotFound:
                    return BattleCommandOutcome.RoomNotFound;
                case BattleLoadResultCode.RoomNotOpen:
                    return BattleCommandOutcome.RoomNotOpen;
                case BattleLoadResultCode.NotInRoom:
                    return BattleCommandOutcome.NotInRoom;
                case BattleLoadResultCode.NotHost:
                    return BattleCommandOutcome.NotHost;
                case BattleLoadResultCode.NotEnoughPlayers:
                    return BattleCommandOutcome.NotEnoughPlayers;
                case BattleLoadResultCode.NotAllReady:
                    return BattleCommandOutcome.NotAllReady;
                case BattleLoadResultCode.StaleSession:
                    return BattleCommandOutcome.StaleSession;
                case BattleLoadResultCode.Overloaded:
                    return BattleCommandOutcome.Overloaded;
                default:
                    return BattleCommandOutcome.Rejected;
            }
        }

        public async Task<BattleCommandHandle> CompleteArenaLoadAsync(
            ulong roomId,
            ulong battleInstanceId,
            CancellationToken cancellationToken)
        {
            ulong requestId = NextRequestId();
            BattleCommandHandle handle = correlator.Register(requestId);
            await SendAsync(
                    handle,
                    BattleLoadProtocolCodec.EncodeArenaLoadComplete(
                        requestId,
                        roomId,
                        battleInstanceId),
                    cancellationToken)
                .ConfigureAwait(false);
            return handle;
        }

        private ulong NextRequestId()
        {
            long value = Interlocked.Increment(ref nextRequestId);
            if (value <= 0)
            {
                throw new InvalidOperationException("Battle request id space exhausted");
            }
            return (ulong)value;
        }

        private async Task SendAsync(
            BattleCommandHandle handle,
            byte[] frame,
            CancellationToken cancellationToken)
        {
            try
            {
                await sender.SendAsync(frame, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception error)
            {
                correlator.Fail(handle.RequestId, error);
                throw;
            }
        }
    }

    public sealed class BattleLoadReadModel
    {
        private IReadOnlyList<BattleParticipant> participants = Array.Empty<BattleParticipant>();

        public event Action Changed;

        public bool IsWaiting { get; private set; }
        public bool IsGameplayActive { get; private set; }
        public ulong RoomId { get; private set; }
        public ulong BattleInstanceId { get; private set; }
        public IReadOnlyList<BattleParticipant> Participants => participants;
        public ArenaLoadCancelReason? LastCancelReason { get; private set; }
        public BattleRecoveryReason? LastRecoveryReason { get; private set; }
        public bool HasLoadFailure => LastCancelReason.HasValue;

        public bool Apply(BattleLoadServerMessage message)
        {
            switch (message)
            {
                case ArenaLoadEntry entry:
                    RoomId = entry.RoomId;
                    BattleInstanceId = entry.BattleInstanceId;
                    participants = Array.Empty<BattleParticipant>();
                    IsWaiting = true;
                    IsGameplayActive = false;
                    LastCancelReason = null;
                    LastRecoveryReason = null;
                    Changed?.Invoke();
                    return true;
                case ArenaGameplayStart start when Matches(start.RoomId, start.BattleInstanceId):
                    participants = start.Participants;
                    IsWaiting = false;
                    IsGameplayActive = true;
                    LastCancelReason = null;
                    Changed?.Invoke();
                    return true;
                case ArenaLoadCancelled cancelled
                    when Matches(cancelled.RoomId, cancelled.BattleInstanceId):
                    participants = Array.Empty<BattleParticipant>();
                    IsWaiting = false;
                    IsGameplayActive = false;
                    LastCancelReason = cancelled.Reason;
                    Changed?.Invoke();
                    return true;
                default:
                    return false;
            }
        }

        public bool Apply(BattleRecoveryNotice notice)
        {
            if (notice == null)
            {
                throw new ArgumentNullException(nameof(notice));
            }
            if (LastRecoveryReason.HasValue ||
                notice.RoomId != RoomId ||
                notice.BattleInstanceId != BattleInstanceId ||
                (!IsWaiting && !IsGameplayActive))
            {
                return false;
            }
            LastRecoveryReason = notice.Reason;
            IsWaiting = true;
            IsGameplayActive = false;
            Changed?.Invoke();
            return true;
        }

        public bool ApplyFinalResult(ulong roomId, ulong battleInstanceId)
        {
            if (roomId != RoomId || battleInstanceId != BattleInstanceId ||
                (!IsWaiting && !IsGameplayActive &&
                 LastRecoveryReason !=
                    BattleRecoveryReason.SettlementRecoveryPending))
            {
                return false;
            }
            LastRecoveryReason = null;
            IsWaiting = false;
            IsGameplayActive = false;
            Changed?.Invoke();
            return true;
        }

        public bool ResetForLobby()
        {
            if (RoomId == 0 && BattleInstanceId == 0)
            {
                return false;
            }
            participants = Array.Empty<BattleParticipant>();
            IsWaiting = false;
            IsGameplayActive = false;
            RoomId = 0;
            BattleInstanceId = 0;
            LastCancelReason = null;
            LastRecoveryReason = null;
            Changed?.Invoke();
            return true;
        }

        private bool Matches(ulong roomId, ulong battleInstanceId)
        {
            return IsWaiting && RoomId == roomId && BattleInstanceId == battleInstanceId;
        }
    }

    public sealed class BattleLoadMessageRouter : ITcpInboundMessageSink
    {
        private readonly BattleResponseCorrelator correlator;
        private readonly BattleLoadReadModel readModel;

        public BattleLoadMessageRouter(
            BattleResponseCorrelator correlator,
            BattleLoadReadModel readModel)
        {
            this.correlator = correlator ?? throw new ArgumentNullException(nameof(correlator));
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
        }

        public void OnMessage(BattleLoadServerMessage message)
        {
            if (message == null)
            {
                throw new ArgumentNullException(nameof(message));
            }
            if (message is BattleCommandResponse response)
            {
                correlator.TryComplete(response.RequestId, response.Result);
                return;
            }
            readModel.Apply(message);
        }
    }
}
