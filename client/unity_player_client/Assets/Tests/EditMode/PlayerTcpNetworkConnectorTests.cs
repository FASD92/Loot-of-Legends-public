using System;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerTcpNetworkConnectorTests
    {
        [Test]
        [Timeout(5000)]
        public async Task ConnectAsyncReadsWelcomeAndInitialRoomListSnapshot()
        {
            byte[] welcome = WelcomePacket(0x0102030405060708UL);
            byte[] snapshot = RoomListSnapshotPacket(
                new PlayerRoomListEntry(17U, 1, 10));

            PlayerNetworkConnectResult result = await ConnectToServerAsync(welcome, snapshot);

            Assert.AreEqual(0x0102030405060708UL, result.SessionId);
            Assert.AreEqual(1, result.ClientSessionCount);
            Assert.IsTrue(result.IsSelfListed);
            Assert.AreEqual(1, result.InitialRoomListCount);
            Assert.AreEqual(17U, result.InitialRoomListEntryAt(0).RoomId);
        }

        [Test]
        [Timeout(5000)]
        public async Task ConnectAsyncSendsAuthenticateGameSessionBeforeReadingWelcome()
        {
            byte[] welcome = WelcomePacket(0x0102030405060708UL);
            byte[] snapshot = RoomListSnapshotPacket();

            byte[] sentAuth = await ConnectAndCaptureAuthenticatePacketAsync(
                "dev-session:player1",
                welcome,
                snapshot);

            Assert.AreEqual(
                PlayerTcpPacket.SerializeAuthenticateGameSession("dev-session:player1"),
                sentAuth);
        }

        [Test]
        [Timeout(5000)]
        public async Task ConnectAsyncRejectsSnapshotMissingWelcomeSession()
        {
            byte[] welcome = WelcomePacket(1UL);
            byte[] snapshot = ClientListSnapshotPacket(2UL);

            InvalidOperationException exception = null;
            try
            {
                await ConnectToServerAsync(welcome, snapshot);
            }
            catch (InvalidOperationException ex)
            {
                exception = ex;
            }

            Assert.IsNotNull(exception);
            Assert.AreEqual("invalid initial room list snapshot packet", exception.Message);
        }

        [Test]
        [Timeout(5000)]
        public async Task SendPacketAsyncWritesPacketToConnectedStream()
        {
            byte[] welcome = WelcomePacket(0x0102030405060708UL);
            byte[] snapshot = RoomListSnapshotPacket();
            byte[] request = PlayerTcpPacket.SerializeCreateRoomRequest();

            TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            int port = ((IPEndPoint)listener.LocalEndpoint).Port;
            Task<byte[]> serverTask = ExchangeAndReadClientPacketAsync(
                listener,
                request.Length,
                welcome,
                snapshot);
            PlayerTcpNetworkConnector connector = new PlayerTcpNetworkConnector();
            try
            {
                Assert.IsTrue(PlayerServerEndpoint.TryCreate(
                    "127.0.0.1",
                    port,
                    port,
                    out PlayerServerEndpoint endpoint));

                await connector.ConnectAsync(
                    endpoint,
                    "dev-session:player1",
                    CancellationToken.None);
                await connector.SendPacketAsync(request, CancellationToken.None);

                byte[] sent = await serverTask;
                Assert.AreEqual(request, sent);
            }
            finally
            {
                connector.Dispose();
                listener.Stop();
            }
        }

        [Test]
        [Timeout(5000)]
        public async Task ReceivePacketAsyncReadsServerPacketAfterCommandWrite()
        {
            byte[] welcome = WelcomePacket(0x0102030405060708UL);
            byte[] snapshot = RoomListSnapshotPacket();
            byte[] request = PlayerTcpPacket.SerializeCreateRoomRequest();
            byte[] response = CreateRoomResponsePacket(17U, 1);
            byte[] roomList = RoomListSnapshotPacket(new PlayerRoomListEntry(17U, 1, 10));

            TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            int port = ((IPEndPoint)listener.LocalEndpoint).Port;
            Task<byte[]> serverTask = ExchangeReadClientThenSendAsync(
                listener,
                request.Length,
                new[] { welcome, snapshot },
                new[] { response, roomList });
            PlayerTcpNetworkConnector connector = new PlayerTcpNetworkConnector();
            try
            {
                Assert.IsTrue(PlayerServerEndpoint.TryCreate(
                    "127.0.0.1",
                    port,
                    port,
                    out PlayerServerEndpoint endpoint));

                await connector.ConnectAsync(
                    endpoint,
                    "dev-session:player1",
                    CancellationToken.None);
                await connector.SendPacketAsync(request, CancellationToken.None);
                byte[] capturedResponse = await connector.ReceivePacketAsync(CancellationToken.None);
                byte[] capturedRoomList = await connector.ReceivePacketAsync(CancellationToken.None);
                byte[] sent = await serverTask;

                Assert.AreEqual(request, sent);
                Assert.AreEqual(response, capturedResponse);
                Assert.AreEqual(roomList, capturedRoomList);
            }
            finally
            {
                connector.Dispose();
                listener.Stop();
            }
        }

        private static async Task<PlayerNetworkConnectResult> ConnectToServerAsync(
            byte[] welcome,
            byte[] snapshot)
        {
            TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            int port = ((IPEndPoint)listener.LocalEndpoint).Port;
            Task serverTask = SendPacketsAsync(listener, welcome, snapshot);
            PlayerTcpNetworkConnector connector = new PlayerTcpNetworkConnector();
            try
            {
                Assert.IsTrue(PlayerServerEndpoint.TryCreate(
                    "127.0.0.1",
                    port,
                    port,
                    out PlayerServerEndpoint endpoint));

                return await connector.ConnectAsync(
                    endpoint,
                    "dev-session:player1",
                    CancellationToken.None);
            }
            finally
            {
                connector.Dispose();
                listener.Stop();
                Task completed = await Task.WhenAny(serverTask, Task.Delay(1000));
                if (completed != serverTask)
                {
                    throw new TimeoutException("tcp connector test server did not finish");
                }

                await serverTask;
            }
        }

        private static async Task<byte[]> ConnectAndCaptureAuthenticatePacketAsync(
            string gameSessionToken,
            byte[] welcome,
            byte[] snapshot)
        {
            TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            int port = ((IPEndPoint)listener.LocalEndpoint).Port;
            Task<byte[]> serverTask =
                ReadAuthenticateThenSendPacketsAsync(listener, welcome, snapshot);
            PlayerTcpNetworkConnector connector = new PlayerTcpNetworkConnector();
            try
            {
                Assert.IsTrue(PlayerServerEndpoint.TryCreate(
                    "127.0.0.1",
                    port,
                    port,
                    out PlayerServerEndpoint endpoint));

                await connector.ConnectAsync(
                    endpoint,
                    gameSessionToken,
                    CancellationToken.None);
            }
            finally
            {
                connector.Dispose();
                listener.Stop();
            }

            return await serverTask;
        }

        private static async Task<byte[]> ReadAuthenticateThenSendPacketsAsync(
            TcpListener listener,
            params byte[][] packets)
        {
            using (TcpClient accepted = await listener.AcceptTcpClientAsync().ConfigureAwait(false))
            {
                NetworkStream stream = accepted.GetStream();
                byte[] header = await ReadExactAsync(stream, PlayerTcpPacket.HeaderSize)
                    .ConfigureAwait(false);
                int packetSize = (header[0] << 8) | header[1];
                byte[] authPacket = new byte[packetSize];
                Buffer.BlockCopy(header, 0, authPacket, 0, header.Length);
                byte[] payload = await ReadExactAsync(
                        stream,
                        packetSize - PlayerTcpPacket.HeaderSize)
                    .ConfigureAwait(false);
                Buffer.BlockCopy(
                    payload,
                    0,
                    authPacket,
                    PlayerTcpPacket.HeaderSize,
                    payload.Length);

                foreach (byte[] packet in packets)
                {
                    await stream.WriteAsync(packet, 0, packet.Length).ConfigureAwait(false);
                }

                return authPacket;
            }
        }

        private static async Task<byte[]> ReadExactAsync(NetworkStream stream, int size)
        {
            byte[] packet = new byte[size];
            int offset = 0;
            while (offset < packet.Length)
            {
                int read = await stream.ReadAsync(packet, offset, packet.Length - offset)
                    .ConfigureAwait(false);
                if (read == 0)
                {
                    throw new InvalidOperationException("client disconnected before packet");
                }

                offset += read;
            }

            return packet;
        }

        private static async Task SendPacketsAsync(TcpListener listener, params byte[][] packets)
        {
            using (TcpClient accepted = await listener.AcceptTcpClientAsync().ConfigureAwait(false))
            {
                NetworkStream stream = accepted.GetStream();
                await ReadAuthenticatePacketAsync(stream).ConfigureAwait(false);
                foreach (byte[] packet in packets)
                {
                    await stream.WriteAsync(packet, 0, packet.Length).ConfigureAwait(false);
                }
            }
        }

        private static async Task<byte[]> ExchangeAndReadClientPacketAsync(
            TcpListener listener,
            int packetSize,
            params byte[][] serverPackets)
        {
            using (TcpClient accepted = await listener.AcceptTcpClientAsync().ConfigureAwait(false))
            {
                NetworkStream stream = accepted.GetStream();
                await ReadAuthenticatePacketAsync(stream).ConfigureAwait(false);
                foreach (byte[] packet in serverPackets)
                {
                    await stream.WriteAsync(packet, 0, packet.Length).ConfigureAwait(false);
                }

                byte[] clientPacket = new byte[packetSize];
                int offset = 0;
                while (offset < clientPacket.Length)
                {
                    int read = await stream.ReadAsync(
                        clientPacket,
                        offset,
                        clientPacket.Length - offset).ConfigureAwait(false);
                    if (read == 0)
                    {
                        throw new InvalidOperationException("client disconnected before packet");
                    }

                    offset += read;
                }

                return clientPacket;
            }
        }

        private static async Task<byte[]> ExchangeReadClientThenSendAsync(
            TcpListener listener,
            int packetSize,
            byte[][] initialServerPackets,
            byte[][] responsePackets)
        {
            using (TcpClient accepted = await listener.AcceptTcpClientAsync().ConfigureAwait(false))
            {
                NetworkStream stream = accepted.GetStream();
                await ReadAuthenticatePacketAsync(stream).ConfigureAwait(false);
                foreach (byte[] packet in initialServerPackets)
                {
                    await stream.WriteAsync(packet, 0, packet.Length).ConfigureAwait(false);
                }

                byte[] clientPacket = new byte[packetSize];
                int offset = 0;
                while (offset < clientPacket.Length)
                {
                    int read = await stream.ReadAsync(
                        clientPacket,
                        offset,
                        clientPacket.Length - offset).ConfigureAwait(false);
                    if (read == 0)
                    {
                        throw new InvalidOperationException("client disconnected before packet");
                    }

                    offset += read;
                }

                foreach (byte[] packet in responsePackets)
                {
                    await stream.WriteAsync(packet, 0, packet.Length).ConfigureAwait(false);
                }

                return clientPacket;
            }
        }

        private static async Task<byte[]> ReadAuthenticatePacketAsync(NetworkStream stream)
        {
            byte[] header = await ReadExactAsync(stream, PlayerTcpPacket.HeaderSize)
                .ConfigureAwait(false);
            int packetSize = (header[0] << 8) | header[1];
            byte[] authPacket = new byte[packetSize];
            Buffer.BlockCopy(header, 0, authPacket, 0, header.Length);
            byte[] payload = await ReadExactAsync(
                    stream,
                    packetSize - PlayerTcpPacket.HeaderSize)
                .ConfigureAwait(false);
            Buffer.BlockCopy(
                payload,
                0,
                authPacket,
                PlayerTcpPacket.HeaderSize,
                payload.Length);
            return authPacket;
        }

        private static byte[] WelcomePacket(ulong sessionId)
        {
            byte[] packet = new byte[12];
            packet[1] = 0x0C;
            packet[3] = 0x01;
            WriteU64BE(sessionId, packet, 4);
            return packet;
        }

        private static byte[] ClientListSnapshotPacket(params ulong[] sessionIds)
        {
            byte[] packet = new byte[6 + (sessionIds.Length * 8)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[3] = 0x02;
            packet[4] = (byte)(sessionIds.Length >> 8);
            packet[5] = (byte)sessionIds.Length;
            int offset = 6;
            foreach (ulong sessionId in sessionIds)
            {
                WriteU64BE(sessionId, packet, offset);
                offset += 8;
            }

            return packet;
        }

        private static byte[] CreateRoomResponsePacket(uint roomId, ushort playerCount)
        {
            byte[] packet = new byte[10];
            packet[1] = 0x0A;
            packet[2] = 0x01;
            packet[3] = 0x02;
            WriteU32BE(roomId, packet, 4);
            packet[8] = (byte)(playerCount >> 8);
            packet[9] = (byte)playerCount;
            return packet;
        }

        private static byte[] RoomListSnapshotPacket(params PlayerRoomListEntry[] rooms)
        {
            int packetSize = 6;
            foreach (PlayerRoomListEntry room in rooms)
            {
                packetSize += 10 + System.Text.Encoding.UTF8.GetByteCount(room.Title ?? string.Empty);
            }

            byte[] packet = new byte[packetSize];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x07;
            packet[4] = (byte)(rooms.Length >> 8);
            packet[5] = (byte)rooms.Length;
            int offset = 6;
            foreach (PlayerRoomListEntry room in rooms)
            {
                WriteU32BE(room.RoomId, packet, offset);
                packet[offset + 4] = (byte)(room.PlayerCount >> 8);
                packet[offset + 5] = (byte)room.PlayerCount;
                packet[offset + 6] = (byte)(room.MaxPlayers >> 8);
                packet[offset + 7] = (byte)room.MaxPlayers;
                packet[offset + 8] = (byte)room.Status;
                byte[] titleBytes = System.Text.Encoding.UTF8.GetBytes(room.Title ?? string.Empty);
                packet[offset + 9] = (byte)titleBytes.Length;
                System.Buffer.BlockCopy(titleBytes, 0, packet, offset + 10, titleBytes.Length);
                offset += 10 + titleBytes.Length;
            }

            return packet;
        }

        private static void WriteU64BE(ulong value, byte[] packet, int offset)
        {
            for (int index = 7; index >= 0; --index)
            {
                packet[offset + index] = (byte)value;
                value >>= 8;
            }
        }

        private static void WriteU32BE(uint value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 24);
            packet[offset + 1] = (byte)(value >> 16);
            packet[offset + 2] = (byte)(value >> 8);
            packet[offset + 3] = (byte)value;
        }
    }
}
