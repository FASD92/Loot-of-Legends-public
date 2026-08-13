using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Transport.Rudp;

namespace LootOfLegends.Transport
{
    public interface ITcpCommandSender
    {
        Task SendAsync(byte[] frame, CancellationToken cancellationToken);
    }

    public interface ITcpInboundMessageSink
    {
        void OnMessage(BattleLoadServerMessage message);
    }

    public interface ISessionInboundMessageSink
    {
        void OnMessage(SessionServerMessage message);
    }

    public interface ILobbyRoomInboundMessageSink
    {
        void OnMessage(LobbyRoomServerMessage message);
    }

    public interface IFinalResultInboundMessageSink
    {
        void OnMessage(BattleFinalResult message);
    }

    public interface IBattleRecoveryInboundMessageSink
    {
        void OnMessage(BattleRecoveryNotice message);
    }

    public interface IRudpBindCapabilitySink
    {
        void OnRudpBindCapability(RudpBindCapability capability);
    }

    public sealed class TcpCommandSender : ITcpCommandSender
    {
        private readonly Stream stream;
        private readonly SemaphoreSlim writeGate = new SemaphoreSlim(1, 1);

        public TcpCommandSender(Stream stream)
        {
            this.stream = stream ?? throw new ArgumentNullException(nameof(stream));
            if (!stream.CanWrite)
            {
                throw new ArgumentException("TCP command stream must be writable", nameof(stream));
            }
        }

        public async Task SendAsync(byte[] frame, CancellationToken cancellationToken)
        {
            if (frame == null || frame.Length == 0)
            {
                throw new ArgumentException("TCP command frame cannot be empty", nameof(frame));
            }

            await writeGate.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                await stream.WriteAsync(frame, 0, frame.Length, cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                writeGate.Release();
            }
        }
    }

    public sealed class TcpInboundPump
    {
        private static readonly ConditionalWeakTable<Stream, RunOwnership> Ownership =
            new ConditionalWeakTable<Stream, RunOwnership>();
        private const int HeaderBytes = 4;
        private const int MinimumPayloadBytes = 5;
        private readonly Stream stream;
        private readonly TypedServerEventRouter router;

        public TcpInboundPump(Stream stream, TypedServerEventRouter router)
        {
            this.stream = stream ?? throw new ArgumentNullException(nameof(stream));
            this.router = router ?? throw new ArgumentNullException(nameof(router));
            if (!stream.CanRead)
            {
                throw new ArgumentException("TCP inbound stream must be readable", nameof(stream));
            }
        }

        public async Task RunAsync(CancellationToken cancellationToken)
        {
            RunOwnership ownership = Ownership.GetOrCreateValue(stream);
            if (Interlocked.CompareExchange(ref ownership.Active, 1, 0) != 0)
            {
                throw new InvalidOperationException(
                    "TCP connection already has an active inbound pump");
            }
            try
            {
                var header = new byte[HeaderBytes];
                while (true)
                {
                    int firstRead = await stream.ReadAsync(
                            header,
                            0,
                            HeaderBytes,
                            cancellationToken)
                        .ConfigureAwait(false);
                    if (firstRead == 0)
                    {
                        return;
                    }
                    await ReadExactAsync(
                            header,
                            firstRead,
                            HeaderBytes - firstRead,
                            cancellationToken)
                        .ConfigureAwait(false);

                    uint payloadLength = ReadUInt32(header, 0);
                    if (payloadLength < MinimumPayloadBytes ||
                        payloadLength > BattleLoadProtocolCodec.MaximumServerPayloadBytes)
                    {
                        throw new BattleLoadProtocolException(
                            "TCP payload length is outside the bounded range");
                    }

                    var frame = new byte[HeaderBytes + payloadLength];
                    Buffer.BlockCopy(header, 0, frame, 0, HeaderBytes);
                    await ReadExactAsync(
                            frame,
                            HeaderBytes,
                            (int)payloadLength,
                            cancellationToken)
                        .ConfigureAwait(false);
                    uint messageId = ReadUInt32(frame, 5);
                    if (messageId >= 2 && messageId <= 4)
                    {
                        router.Dispatch(SessionProtocolCodec.DecodeServerFrame(frame));
                    }
                    else if (messageId == 21)
                    {
                        router.Dispatch(RudpProtocolCodec.DecodeBindCapability(frame));
                    }
                    else if (messageId == 5 || messageId == 6 ||
                             messageId == 12 || messageId == 13)
                    {
                        router.Dispatch(LobbyRoomProtocolCodec.DecodeServerFrame(frame));
                    }
                    else if (messageId == 36)
                    {
                        router.Dispatch(FinalResultProtocolCodec.DecodeServerFrame(frame));
                    }
                    else if (messageId == 37)
                    {
                        router.Dispatch(BattleRecoveryProtocolCodec.DecodeServerFrame(frame));
                    }
                    else
                    {
                        router.Dispatch(BattleLoadProtocolCodec.DecodeServerFrame(frame));
                    }
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
            }
            finally
            {
                Volatile.Write(ref ownership.Active, 0);
            }
        }

        private async Task ReadExactAsync(
            byte[] destination,
            int offset,
            int count,
            CancellationToken cancellationToken)
        {
            while (count > 0)
            {
                int read = await stream.ReadAsync(destination, offset, count, cancellationToken)
                    .ConfigureAwait(false);
                if (read == 0)
                {
                    throw new EndOfStreamException("TCP frame ended before its declared payload");
                }
                offset += read;
                count -= read;
            }
        }

        private static uint ReadUInt32(byte[] bytes, int offset)
        {
            return ((uint)bytes[offset] << 24) |
                   ((uint)bytes[offset + 1] << 16) |
                   ((uint)bytes[offset + 2] << 8) |
                   bytes[offset + 3];
        }

        private sealed class RunOwnership
        {
            public int Active;
        }
    }
}
