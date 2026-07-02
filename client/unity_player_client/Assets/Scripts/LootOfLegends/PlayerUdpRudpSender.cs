using System;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerUdpRudpSender : IPlayerRudpSender
    {
        private readonly object syncRoot = new object();
        private UdpClient udpClient;
        private uint nextSequence = 1U;
        private uint nextCommandSequence = 1U;

        public Task<PlayerRudpHelloSendResult> SendHelloAsync(
            PlayerServerEndpoint endpoint,
            PlayerRudpHello hello,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!endpoint.IsValid)
            {
                throw new InvalidOperationException("invalid endpoint");
            }
            if (!hello.IsValid)
            {
                throw new InvalidOperationException("invalid RUDP Hello");
            }

            UdpClient client = EnsureUdpClient(endpoint.Host);
            uint sequence;
            lock (syncRoot)
            {
                sequence = nextSequence;
                byte[] packet = PlayerRudpPacket.SerializeHello(sequence, hello);
                client.Send(packet, packet.Length, endpoint.Host, endpoint.RudpPort);
                AdvanceSequence();
            }

            return Task.FromResult(
                new PlayerRudpHelloSendResult(hello.SessionId, sequence, LocalEndpointText(client)));
        }

        public Task<PlayerRudpInputCommandSendResult> SendAttackAsync(
            PlayerServerEndpoint endpoint,
            ulong sessionId,
            uint targetHintMonsterId,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!endpoint.IsValid)
            {
                throw new InvalidOperationException("invalid endpoint");
            }

            PlayerRudpHello hello = PlayerRudpHello.ForSession(sessionId);
            if (!hello.IsValid)
            {
                throw new InvalidOperationException("invalid RUDP Attack session");
            }

            UdpClient client = EnsureUdpClient(endpoint.Host);
            uint sequence;
            uint commandSequence;
            lock (syncRoot)
            {
                sequence = nextSequence;
                commandSequence = nextCommandSequence;
                byte[] packet = PlayerRudpPacket.SerializeAttackInputCommand(
                    sequence,
                    hello.ClientId,
                    commandSequence,
                    targetHintMonsterId);
                client.Send(packet, packet.Length, endpoint.Host, endpoint.RudpPort);
                AdvanceSequence();
                AdvanceCommandSequence();
            }

            return Task.FromResult(new PlayerRudpInputCommandSendResult(
                sessionId,
                sequence,
                commandSequence,
                targetHintMonsterId,
                LocalEndpointText(client)));
        }

        public Task<PlayerRudpInputCommandSendResult> SendMoveAsync(
            PlayerServerEndpoint endpoint,
            ulong sessionId,
            short dirX,
            short dirY,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!endpoint.IsValid)
            {
                throw new InvalidOperationException("invalid endpoint");
            }

            PlayerRudpHello hello = PlayerRudpHello.ForSession(sessionId);
            if (!hello.IsValid)
            {
                throw new InvalidOperationException("invalid RUDP Move session");
            }

            UdpClient client = EnsureUdpClient(endpoint.Host);
            uint sequence;
            uint commandSequence;
            lock (syncRoot)
            {
                sequence = nextSequence;
                commandSequence = nextCommandSequence;
                byte[] packet = PlayerRudpPacket.SerializeMoveInputCommand(
                    sequence,
                    hello.ClientId,
                    commandSequence,
                    dirX,
                    dirY);
                client.Send(packet, packet.Length, endpoint.Host, endpoint.RudpPort);
                AdvanceSequence();
                AdvanceCommandSequence();
            }

            return Task.FromResult(new PlayerRudpInputCommandSendResult(
                sessionId,
                sequence,
                commandSequence,
                0U,
                LocalEndpointText(client)));
        }

        public Task<PlayerRudpInputCommandSendResult> SendSpaceLootAsync(
            PlayerServerEndpoint endpoint,
            ulong sessionId,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!endpoint.IsValid)
            {
                throw new InvalidOperationException("invalid endpoint");
            }

            PlayerRudpHello hello = PlayerRudpHello.ForSession(sessionId);
            if (!hello.IsValid)
            {
                throw new InvalidOperationException("invalid RUDP SpaceLoot session");
            }

            UdpClient client = EnsureUdpClient(endpoint.Host);
            uint sequence;
            uint commandSequence;
            lock (syncRoot)
            {
                sequence = nextSequence;
                commandSequence = nextCommandSequence;
                byte[] packet = PlayerRudpPacket.SerializeSpaceLootInputCommand(
                    sequence,
                    hello.ClientId,
                    commandSequence);
                client.Send(packet, packet.Length, endpoint.Host, endpoint.RudpPort);
                AdvanceSequence();
                AdvanceCommandSequence();
            }

            return Task.FromResult(new PlayerRudpInputCommandSendResult(
                sessionId,
                sequence,
                commandSequence,
                0U,
                LocalEndpointText(client)));
        }

        public bool TryReceiveStateSnapshot(out PlayerRudpStateSnapshot snapshot)
        {
            snapshot = default;
            UdpClient client;
            lock (syncRoot)
            {
                client = udpClient;
                if (client == null)
                {
                    return false;
                }

                try
                {
                    if (client.Available <= 0)
                    {
                        return false;
                    }

                    IPEndPoint remoteEndpoint = null;
                    byte[] packet = client.Receive(ref remoteEndpoint);
                    if (!PlayerRudpPacket.TryParsePacket(
                            packet,
                            packet.Length,
                            out PlayerRudpPacketHeader header,
                            out byte[] payload) ||
                        !PlayerRudpPacket.IsStateSnapshot(header))
                    {
                        return false;
                    }

                    return PlayerRudpPacket.TryParseStateSnapshotPayload(
                        payload,
                        payload.Length,
                        out snapshot);
                }
                catch (SocketException)
                {
                    return false;
                }
                catch (ObjectDisposedException)
                {
                    return false;
                }
            }
        }

        public void Disconnect()
        {
            UdpClient clientToClose;
            lock (syncRoot)
            {
                clientToClose = udpClient;
                udpClient = null;
                nextSequence = 1U;
                nextCommandSequence = 1U;
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

        private UdpClient EnsureUdpClient(string host)
        {
            lock (syncRoot)
            {
                if (udpClient != null)
                {
                    return udpClient;
                }

                if (IPAddress.TryParse(host, out IPAddress address) &&
                    address.AddressFamily == AddressFamily.InterNetworkV6)
                {
                    udpClient = new UdpClient(new IPEndPoint(IPAddress.IPv6Any, 0));
                }
                else
                {
                    udpClient = new UdpClient(new IPEndPoint(IPAddress.Any, 0));
                }

                return udpClient;
            }
        }

        private void AdvanceSequence()
        {
            ++nextSequence;
            if (nextSequence == 0U)
            {
                nextSequence = 1U;
            }
        }

        private void AdvanceCommandSequence()
        {
            ++nextCommandSequence;
            if (nextCommandSequence == 0U)
            {
                nextCommandSequence = 1U;
            }
        }

        private static string LocalEndpointText(UdpClient client)
        {
            if (client.Client.LocalEndPoint is IPEndPoint endpoint)
            {
                return endpoint.Address + ":" + endpoint.Port;
            }

            return string.Empty;
        }
    }
}
