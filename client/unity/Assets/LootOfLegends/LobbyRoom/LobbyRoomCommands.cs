using System;
using System.Collections.Concurrent;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;

namespace LootOfLegends.LobbyRoom
{
    public enum RoomCommandResult
    {
        Ok = 0,
        InvalidArgument = 1,
        AlreadyInRoom = 2,
        RoomNotFound = 3,
        RoomClosed = 4,
        RoomFull = 5,
        NotInRoom = 6,
        NotHost = 7,
        NotEnoughPlayers = 8,
        NotAllReady = 9,
        InvalidTarget = 10,
        RoomOverloaded = 11,
        StaleSession = 12
    }

    public interface ILobbyRoomCommands
    {
        Task<RoomCommandResult> CreateAsync(
            string title,
            byte capacity,
            CancellationToken cancellationToken);

        Task<RoomCommandResult> JoinAsync(
            ulong roomId,
            CancellationToken cancellationToken);

        Task<RoomCommandResult> LeaveAsync(CancellationToken cancellationToken);

        Task<RoomCommandResult> SetReadyAsync(
            bool ready,
            CancellationToken cancellationToken);

        Task<RoomCommandResult> KickAsync(
            ulong targetSessionId,
            ulong targetSessionGeneration,
            CancellationToken cancellationToken);
    }

    public sealed class RoomCommandCorrelator
    {
        private readonly ConcurrentDictionary<ulong, TaskCompletionSource<RoomCommandResult>>
            pending = new ConcurrentDictionary<ulong, TaskCompletionSource<RoomCommandResult>>();
        private long orphanedResponseCount;

        public long OrphanedResponseCount => Interlocked.Read(ref orphanedResponseCount);

        public Task<RoomCommandResult> Register(ulong requestId)
        {
            var completion = new TaskCompletionSource<RoomCommandResult>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            if (requestId == 0 || !pending.TryAdd(requestId, completion))
            {
                throw new ArgumentException(
                    "Room request correlation must be unique and non-zero", nameof(requestId));
            }
            return completion.Task;
        }

        public void Complete(ulong requestId, RoomCommandResult result)
        {
            if (pending.TryRemove(
                    requestId,
                    out TaskCompletionSource<RoomCommandResult> completion))
            {
                completion.TrySetResult(result);
                return;
            }
            Interlocked.Increment(ref orphanedResponseCount);
        }

        public void Fail(ulong requestId, Exception error)
        {
            if (pending.TryRemove(
                    requestId,
                    out TaskCompletionSource<RoomCommandResult> completion))
            {
                completion.TrySetException(error);
            }
        }
    }

    public sealed class LobbyRoomMessageRouter : ILobbyRoomInboundMessageSink
    {
        private readonly RoomCommandCorrelator correlator;
        private readonly LobbyRoomReadModel readModel;

        public LobbyRoomMessageRouter(
            RoomCommandCorrelator correlator,
            LobbyRoomReadModel readModel)
        {
            this.correlator = correlator ?? throw new ArgumentNullException(nameof(correlator));
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
        }

        public void OnMessage(LobbyRoomServerMessage message)
        {
            if (message == null)
            {
                throw new ArgumentNullException(nameof(message));
            }
            if (message is RoomCommandResponse response)
            {
                correlator.Complete(response.RequestId, (RoomCommandResult)response.ResultCode);
                return;
            }
            readModel.Apply(message);
        }
    }

    public sealed class LobbyRoomCommandCoordinator : ILobbyRoomCommands
    {
        private readonly ITcpCommandSender sender;
        private readonly RoomCommandCorrelator correlator;
        private long nextRequestId;

        public LobbyRoomCommandCoordinator(
            ITcpCommandSender sender,
            RoomCommandCorrelator correlator)
        {
            this.sender = sender ?? throw new ArgumentNullException(nameof(sender));
            this.correlator = correlator ?? throw new ArgumentNullException(nameof(correlator));
        }

        public Task<RoomCommandResult> CreateAsync(
            string title,
            byte capacity,
            CancellationToken cancellationToken)
        {
            ulong requestId = NextRequestId();
            return SendAsync(
                requestId,
                LobbyRoomProtocolCodec.EncodeCreateRoom(requestId, title, capacity),
                cancellationToken);
        }

        public Task<RoomCommandResult> JoinAsync(
            ulong roomId,
            CancellationToken cancellationToken)
        {
            ulong requestId = NextRequestId();
            return SendAsync(
                requestId,
                LobbyRoomProtocolCodec.EncodeJoinRoom(requestId, roomId),
                cancellationToken);
        }

        public Task<RoomCommandResult> LeaveAsync(CancellationToken cancellationToken)
        {
            ulong requestId = NextRequestId();
            return SendAsync(
                requestId,
                LobbyRoomProtocolCodec.EncodeLeaveRoom(requestId),
                cancellationToken);
        }

        public Task<RoomCommandResult> SetReadyAsync(
            bool ready,
            CancellationToken cancellationToken)
        {
            ulong requestId = NextRequestId();
            return SendAsync(
                requestId,
                LobbyRoomProtocolCodec.EncodeSetReady(requestId, ready),
                cancellationToken);
        }

        public Task<RoomCommandResult> KickAsync(
            ulong targetSessionId,
            ulong targetSessionGeneration,
            CancellationToken cancellationToken)
        {
            ulong requestId = NextRequestId();
            return SendAsync(
                requestId,
                LobbyRoomProtocolCodec.EncodeKickRoomMember(
                    requestId,
                    targetSessionId,
                    targetSessionGeneration),
                cancellationToken);
        }

        private ulong NextRequestId()
        {
            long value = Interlocked.Increment(ref nextRequestId);
            if (value <= 0)
            {
                throw new InvalidOperationException("Room request id space exhausted");
            }
            return (ulong)value;
        }

        private async Task<RoomCommandResult> SendAsync(
            ulong requestId,
            byte[] frame,
            CancellationToken cancellationToken)
        {
            Task<RoomCommandResult> completion = correlator.Register(requestId);
            try
            {
                await sender.SendAsync(frame, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception error)
            {
                correlator.Fail(requestId, error);
                throw;
            }
            return await completion.ConfigureAwait(false);
        }
    }
}
