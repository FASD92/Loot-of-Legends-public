using System;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerTcpNetworkConnector : IPlayerNetworkConnector
    {
        private readonly object syncRoot = new object();
        private TcpClient client;

        public bool HasPendingPacket
        {
            get
            {
                TcpClient currentClient;
                lock (syncRoot)
                {
                    currentClient = client;
                }

                return currentClient != null &&
                    currentClient.Connected &&
                    currentClient.GetStream().DataAvailable;
            }
        }

        public async Task<PlayerNetworkConnectResult> ConnectAsync(
            PlayerServerEndpoint endpoint,
            string gameSessionToken,
            CancellationToken cancellationToken)
        {
            Disconnect();

            TcpClient nextClient = new TcpClient();
            try
            {
                using (cancellationToken.Register(() => nextClient.Close()))
                {
                    await nextClient.ConnectAsync(endpoint.Host, endpoint.TcpPort);
                    cancellationToken.ThrowIfCancellationRequested();

                    nextClient.NoDelay = true;
                    NetworkStream stream = nextClient.GetStream();

                    byte[] authPacket =
                        PlayerTcpPacket.SerializeAuthenticateGameSession(gameSessionToken);
                    await stream.WriteAsync(
                        authPacket,
                        0,
                        authPacket.Length,
                        cancellationToken);
                    cancellationToken.ThrowIfCancellationRequested();

                    byte[] welcomePacket = await ReadTcpPacketAsync(
                        stream,
                        cancellationToken,
                        "tcp disconnected before welcome");
                    cancellationToken.ThrowIfCancellationRequested();

                    if (!PlayerTcpPacket.TryParseWelcome(welcomePacket, out ulong sessionId))
                    {
                        throw new InvalidOperationException("invalid welcome packet");
                    }

                    byte[] roomListPacket = await ReadTcpPacketAsync(
                        stream,
                        cancellationToken,
                        "tcp disconnected before initial room list snapshot");
                    cancellationToken.ThrowIfCancellationRequested();

                    if (!PlayerTcpPacket.TryParseRoomListSnapshot(
                            roomListPacket,
                            out PlayerRoomListSnapshot snapshot))
                    {
                        throw new InvalidOperationException(
                            "invalid initial room list snapshot packet");
                    }

                    lock (syncRoot)
                    {
                        client = nextClient;
                        nextClient = null;
                    }

                    return new PlayerNetworkConnectResult(sessionId, snapshot);
                }
            }
            finally
            {
                if (nextClient != null)
                {
                    nextClient.Close();
                }
            }
        }

        public async Task SendPacketAsync(byte[] packet, CancellationToken cancellationToken)
        {
            if (packet == null ||
                packet.Length < PlayerTcpPacket.HeaderSize ||
                packet.Length > PlayerTcpPacket.MaxPacketSize)
            {
                throw new ArgumentException("invalid TCP packet", nameof(packet));
            }

            byte[] packetCopy = new byte[packet.Length];
            Buffer.BlockCopy(packet, 0, packetCopy, 0, packet.Length);

            TcpClient currentClient;
            lock (syncRoot)
            {
                currentClient = client;
            }
            if (currentClient == null)
            {
                throw new InvalidOperationException("tcp not connected");
            }

            await currentClient.GetStream().WriteAsync(
                packetCopy,
                0,
                packetCopy.Length,
                cancellationToken);
        }

        public Task<byte[]> ReceivePacketAsync(CancellationToken cancellationToken)
        {
            TcpClient currentClient;
            lock (syncRoot)
            {
                currentClient = client;
            }
            if (currentClient == null)
            {
                throw new InvalidOperationException("tcp not connected");
            }

            return ReadTcpPacketAsync(
                currentClient.GetStream(),
                cancellationToken,
                "tcp disconnected before packet");
        }

        private static async Task<byte[]> ReadTcpPacketAsync(
            NetworkStream stream,
            CancellationToken cancellationToken,
            string disconnectMessage)
        {
            byte[] header = new byte[PlayerTcpPacket.HeaderSize];
            await ReadExactlyAsync(
                stream,
                header,
                0,
                header.Length,
                cancellationToken,
                disconnectMessage);

            int size = (header[0] << 8) | header[1];
            if (size < PlayerTcpPacket.HeaderSize || size > PlayerTcpPacket.MaxPacketSize)
            {
                throw new InvalidOperationException("invalid tcp packet size");
            }

            byte[] packet = new byte[size];
            Buffer.BlockCopy(header, 0, packet, 0, header.Length);
            if (size > header.Length)
            {
                await ReadExactlyAsync(
                    stream,
                    packet,
                    header.Length,
                    size - header.Length,
                    cancellationToken,
                    disconnectMessage);
            }

            return packet;
        }

        private static async Task ReadExactlyAsync(
            NetworkStream stream,
            byte[] buffer,
            int offset,
            int length,
            CancellationToken cancellationToken,
            string disconnectMessage)
        {
            int end = offset + length;
            while (offset < end)
            {
                int read = await stream.ReadAsync(
                    buffer,
                    offset,
                    end - offset,
                    cancellationToken);
                if (read == 0)
                {
                    throw new InvalidOperationException(disconnectMessage);
                }

                offset += read;
            }
        }

        public void Disconnect()
        {
            TcpClient clientToClose;
            lock (syncRoot)
            {
                clientToClose = client;
                client = null;
            }

            if (clientToClose != null)
            {
                clientToClose.Close();
            }
        }

        public void Dispose()
        {
            Disconnect();
        }
    }
}
