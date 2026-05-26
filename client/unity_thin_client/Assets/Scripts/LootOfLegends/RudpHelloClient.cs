using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.ThinClient
{
    internal sealed class RudpHelloClient : IDisposable
    {
        private const ushort ClientVersion = 1;
        private const double HelloHeartbeatIntervalSeconds = 3.0;
        private const double StateSnapshotLogIntervalSeconds = 1.0;

        private readonly object syncRoot = new object();
        private readonly object sendRoot = new object();
        private readonly Queue<string> pendingLogLines = new Queue<string>();
        private readonly uint clientId;
        private UdpClient udpClient;
        private CancellationTokenSource receiveCancellation;
        private uint nextSequence = 1;
        private uint nextCommandSequence = 1;
        private bool helloSent;
        private ulong helloSessionId;
        private ulong helloHeartbeatSentCount;
        private DateTime lastHelloHeartbeatSentAtUtc;
        private DateTime nextHelloHeartbeatAtUtc;
        private bool hasReceiveAck;
        private uint receiveAck;
        private uint receiveAckBits;
        private bool battleStartLogged;
        private bool hasLatestStateSnapshot;
        private RudpPacketCodec.RudpStateSnapshotPayload latestStateSnapshot;
        private ulong stateSnapshotReceivedCount;
        private DateTime latestStateSnapshotReceivedAtUtc;
        private DateTime lastStateSnapshotLogAtUtc;
        private uint lastLoggedStateSnapshotTick;
        private bool hasLatestLootResolved;
        private RudpPacketCodec.RudpLootResolvedGameEventPayload latestLootResolved;
        private ulong lootResolvedReceivedCount;

        public RudpHelloClient(uint clientId)
        {
            this.clientId = clientId;
        }

        public bool SendHello(string host, ushort port, ulong sessionId, out string message)
        {
            if (sessionId == 0)
            {
                message = "RUDP Hello blocked: missing TCP Welcome sessionId";
                return false;
            }

            try
            {
                UdpClient client = EnsureUdpClient(host);
                uint sequence;
                lock (sendRoot)
                {
                    sequence = nextSequence;
                    byte[] packet = RudpPacketCodec.SerializeHello(sequence, ClientVersion, clientId, sessionId);
                    client.Send(packet, packet.Length, host, port);
                    AdvancePacketSequence();
                }
                lock (syncRoot)
                {
                    DateTime now = DateTime.UtcNow;
                    helloSent = true;
                    helloSessionId = sessionId;
                    helloHeartbeatSentCount = 0;
                    lastHelloHeartbeatSentAtUtc = DateTime.MinValue;
                    nextHelloHeartbeatAtUtc = now.AddSeconds(HelloHeartbeatIntervalSeconds);
                }

                message = "RUDP Hello sent(clientId=" + clientId +
                          ", sessionId=" + sessionId +
                          ", sequence=" + sequence +
                          ", local=" + LocalEndpointText(client) + ")";
                return true;
            }
            catch (Exception ex) when (ex is SocketException || ex is ObjectDisposedException || ex is InvalidOperationException)
            {
                message = "RUDP Hello failed: " + ex.Message;
                ResetEndpoint();
                return false;
            }
        }

        public bool TickHelloHeartbeat(
            string host,
            ushort port,
            ulong sessionId,
            out string message)
        {
            message = string.Empty;
            if (sessionId == 0)
            {
                return false;
            }

            DateTime now = DateTime.UtcNow;
            lock (syncRoot)
            {
                if (!helloSent ||
                    udpClient == null ||
                    helloSessionId != sessionId ||
                    now < nextHelloHeartbeatAtUtc)
                {
                    return false;
                }
            }

            try
            {
                UdpClient client = EnsureUdpClient(host);
                lock (sendRoot)
                {
                    uint sequence = nextSequence;
                    byte[] packet = RudpPacketCodec.SerializeHello(
                        sequence,
                        ClientVersion,
                        clientId,
                        sessionId);
                    client.Send(packet, packet.Length, host, port);
                    AdvancePacketSequence();
                }

                lock (syncRoot)
                {
                    ++helloHeartbeatSentCount;
                    lastHelloHeartbeatSentAtUtc = now;
                    nextHelloHeartbeatAtUtc = now.AddSeconds(HelloHeartbeatIntervalSeconds);
                }
                return true;
            }
            catch (Exception ex) when (ex is SocketException || ex is ObjectDisposedException || ex is InvalidOperationException)
            {
                message = "RUDP Hello heartbeat failed: " + ex.Message;
                ResetEndpoint();
                return true;
            }
        }

        public bool SendReadyInputCommand(string host, ushort port, ulong sessionId, out string message)
        {
            if (sessionId == 0)
            {
                message = "RUDP Ready blocked: missing TCP Welcome sessionId";
                return false;
            }
            lock (syncRoot)
            {
                if (!helloSent || udpClient == null)
                {
                    message = "RUDP Ready blocked: send RUDP Hello first";
                    return false;
                }
                if (helloSessionId != sessionId)
                {
                    message = "RUDP Ready blocked: send RUDP Hello for current session first";
                    return false;
                }
            }

            try
            {
                UdpClient client = EnsureUdpClient(host);
                uint sequence;
                uint commandSequence;
                lock (sendRoot)
                {
                    sequence = nextSequence;
                    commandSequence = nextCommandSequence;
                    byte[] packet = RudpPacketCodec.SerializeReadyInputCommand(
                        sequence,
                        clientId,
                        commandSequence);
                    client.Send(packet, packet.Length, host, port);
                    AdvancePacketSequence();
                    AdvanceCommandSequence();
                }

                message = "RUDP Ready InputCommand sent(clientId=" + clientId +
                          ", cmdSeq=" + commandSequence +
                          ", sequence=" + sequence +
                          ", local=" + LocalEndpointText(client) + ")";
                return true;
            }
            catch (Exception ex) when (ex is SocketException || ex is ObjectDisposedException || ex is InvalidOperationException)
            {
                message = "RUDP Ready failed: " + ex.Message;
                ResetEndpoint();
                return false;
            }
        }

        public bool SendMoveInputCommand(
            string host,
            ushort port,
            ulong sessionId,
            short dirX,
            short dirY,
            out string message)
        {
            if (sessionId == 0)
            {
                message = "RUDP Move blocked: missing TCP Welcome sessionId";
                return false;
            }
            lock (syncRoot)
            {
                if (!helloSent || udpClient == null)
                {
                    message = "RUDP Move blocked: send RUDP Hello first";
                    return false;
                }
                if (helloSessionId != sessionId)
                {
                    message = "RUDP Move blocked: send RUDP Hello for current session first";
                    return false;
                }
            }

            try
            {
                UdpClient client = EnsureUdpClient(host);
                uint sequence;
                uint commandSequence;
                lock (sendRoot)
                {
                    sequence = nextSequence;
                    commandSequence = nextCommandSequence;
                    byte[] packet = RudpPacketCodec.SerializeMoveInputCommand(
                        sequence,
                        clientId,
                        commandSequence,
                        dirX,
                        dirY);
                    client.Send(packet, packet.Length, host, port);
                    AdvancePacketSequence();
                    AdvanceCommandSequence();
                }

                message = "RUDP Move InputCommand sent(clientId=" + clientId +
                          ", cmdSeq=" + commandSequence +
                          ", dir=" + dirX + "/" + dirY +
                          ", sequence=" + sequence +
                          ", local=" + LocalEndpointText(client) + ")";
                return true;
            }
            catch (Exception ex) when (ex is SocketException || ex is ObjectDisposedException || ex is InvalidOperationException)
            {
                message = "RUDP Move failed: " + ex.Message;
                ResetEndpoint();
                return false;
            }
        }

        public void DrainLogLines(List<string> outLogLines)
        {
            lock (syncRoot)
            {
                while (pendingLogLines.Count > 0)
                {
                    outLogLines.Add(pendingLogLines.Dequeue());
                }
            }
        }

        public bool TryLatestStateSnapshot(out RudpPacketCodec.RudpStateSnapshotPayload snapshot)
        {
            lock (syncRoot)
            {
                if (!hasLatestStateSnapshot)
                {
                    snapshot = new RudpPacketCodec.RudpStateSnapshotPayload
                    {
                        Players = new RudpPacketCodec.RudpStateSnapshotPlayer[0]
                    };
                    return false;
                }

                snapshot = CopyStateSnapshot(latestStateSnapshot);
                return true;
            }
        }

        public bool TryLatestLootResolved(out RudpPacketCodec.RudpLootResolvedGameEventPayload payload)
        {
            lock (syncRoot)
            {
                payload = latestLootResolved;
                return hasLatestLootResolved;
            }
        }

        public HelloHeartbeatStats HeartbeatStats()
        {
            lock (syncRoot)
            {
                DateTime now = DateTime.UtcNow;
                bool active = helloSent && udpClient != null && helloSessionId != 0;
                double lastAgeSeconds = -1.0;
                if (active && helloHeartbeatSentCount > 0)
                {
                    lastAgeSeconds = Math.Max(
                        0.0,
                        (now - lastHelloHeartbeatSentAtUtc).TotalSeconds);
                }

                double nextDueSeconds = -1.0;
                if (active)
                {
                    nextDueSeconds = Math.Max(
                        0.0,
                        (nextHelloHeartbeatAtUtc - now).TotalSeconds);
                }

                return new HelloHeartbeatStats
                {
                    Active = active,
                    SentCount = helloHeartbeatSentCount,
                    LastAgeSeconds = lastAgeSeconds,
                    NextDueSeconds = nextDueSeconds
                };
            }
        }

        public StateSnapshotStats SnapshotStats()
        {
            lock (syncRoot)
            {
                double ageSeconds = -1.0;
                if (hasLatestStateSnapshot)
                {
                    ageSeconds = Math.Max(
                        0.0,
                        (DateTime.UtcNow - latestStateSnapshotReceivedAtUtc).TotalSeconds);
                }

                return new StateSnapshotStats
                {
                    HasSnapshot = hasLatestStateSnapshot,
                    RoomId = hasLatestStateSnapshot ? latestStateSnapshot.RoomId : 0,
                    ServerTick = hasLatestStateSnapshot ? latestStateSnapshot.ServerTick : 0,
                    PlayerCount = hasLatestStateSnapshot && latestStateSnapshot.Players != null
                        ? latestStateSnapshot.Players.Length
                        : 0,
                    ReceivedCount = stateSnapshotReceivedCount,
                    AgeSeconds = ageSeconds,
                    LastLoggedTick = lastLoggedStateSnapshotTick
                };
            }
        }

        public void ResetEndpoint()
        {
            CancellationTokenSource cancellationToStop;
            UdpClient clientToClose;
            lock (syncRoot)
            {
                cancellationToStop = receiveCancellation;
                receiveCancellation = null;
                clientToClose = udpClient;
                udpClient = null;
                helloSent = false;
                helloSessionId = 0;
                helloHeartbeatSentCount = 0;
                lastHelloHeartbeatSentAtUtc = DateTime.MinValue;
                nextHelloHeartbeatAtUtc = DateTime.MinValue;
                hasReceiveAck = false;
                receiveAck = 0;
                receiveAckBits = 0;
                battleStartLogged = false;
                hasLatestStateSnapshot = false;
                latestStateSnapshot = new RudpPacketCodec.RudpStateSnapshotPayload
                {
                    Players = new RudpPacketCodec.RudpStateSnapshotPlayer[0]
                };
                stateSnapshotReceivedCount = 0;
                latestStateSnapshotReceivedAtUtc = DateTime.MinValue;
                lastStateSnapshotLogAtUtc = DateTime.MinValue;
                lastLoggedStateSnapshotTick = 0;
                hasLatestLootResolved = false;
                latestLootResolved = new RudpPacketCodec.RudpLootResolvedGameEventPayload();
                lootResolvedReceivedCount = 0;
            }

            if (cancellationToStop != null)
            {
                cancellationToStop.Cancel();
                cancellationToStop.Dispose();
            }
            if (clientToClose != null)
            {
                clientToClose.Close();
            }
        }

        public void Dispose()
        {
            ResetEndpoint();
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
                udpClient.Client.ReceiveTimeout = 250;

                receiveCancellation = new CancellationTokenSource();
                UdpClient clientForLoop = udpClient;
                CancellationToken token = receiveCancellation.Token;
                Task.Run(() => RunReceiveLoop(clientForLoop, token), token);

                return udpClient;
            }
        }

        private void RunReceiveLoop(UdpClient client, CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    IPEndPoint remoteEndpoint = ReceiveAnyEndpoint(client);
                    byte[] datagram = client.Receive(ref remoteEndpoint);
                    HandleReceivedDatagram(client, remoteEndpoint, datagram);
                }
                catch (SocketException ex)
                {
                    if (token.IsCancellationRequested)
                    {
                        break;
                    }
                    if (ex.SocketErrorCode != SocketError.TimedOut &&
                        ex.SocketErrorCode != SocketError.WouldBlock)
                    {
                        EnqueueLog("RUDP receive failed: " + ex.Message);
                        break;
                    }
                }
                catch (ObjectDisposedException)
                {
                    break;
                }
                catch (InvalidOperationException ex)
                {
                    if (!token.IsCancellationRequested)
                    {
                        EnqueueLog("RUDP receive failed: " + ex.Message);
                    }
                    break;
                }
            }
        }

        private void HandleReceivedDatagram(
            UdpClient client,
            IPEndPoint remoteEndpoint,
            byte[] datagram)
        {
            if (!RudpPacketCodec.TryParsePacket(
                    datagram,
                    datagram.Length,
                    out RudpPacketCodec.RudpPacketHeader header,
                    out byte[] payload,
                    out string error))
            {
                EnqueueLog("RUDP malformed packet: " + error);
                return;
            }

            if (RudpPacketCodec.IsStateSnapshot(header))
            {
                if (!RudpPacketCodec.TryParseStateSnapshotPayload(
                        payload,
                        payload.Length,
                        out RudpPacketCodec.RudpStateSnapshotPayload snapshot,
                        out error))
                {
                    EnqueueLog("RUDP StateSnapshot decode failed: " + error);
                    return;
                }

                if (TryStoreLatestStateSnapshot(snapshot, out bool shouldLog, out ulong receivedCount))
                {
                    if (shouldLog)
                    {
                        EnqueueLog("RUDP StateSnapshot(roomId=" + snapshot.RoomId +
                                   ", tick=" + snapshot.ServerTick +
                                   ", players=" + snapshot.Players.Length +
                                   ", received=" + receivedCount + ")");
                    }
                }
                return;
            }

            if (!RudpPacketCodec.IsReliable(header))
            {
                EnqueueLog("RUDP ignored unreliable packet(type=0x" + header.PacketType.ToString("X4") + ")");
                return;
            }

            ReceiveRecordResult recordResult = RecordReliableSequence(header.Sequence);
            if (TryCurrentAck(out uint ack, out uint ackBits))
            {
                SendAckOnly(client, remoteEndpoint, ack, ackBits);
            }

            if (recordResult == ReceiveRecordResult.Duplicate ||
                recordResult == ReceiveRecordResult.TooOld)
            {
                return;
            }

            if (RudpPacketCodec.IsBattleStart(header))
            {
                if (!RudpPacketCodec.TryParseBattleStartPayload(
                        payload,
                        payload.Length,
                        out RudpPacketCodec.RudpBattleStartPayload battleStart,
                        out error))
                {
                    EnqueueLog("RUDP BattleStart decode failed: " + error);
                    return;
                }

                bool shouldLogBattleStart;
                lock (syncRoot)
                {
                    shouldLogBattleStart = !battleStartLogged;
                    battleStartLogged = true;
                }
                if (!shouldLogBattleStart)
                {
                    return;
                }

                EnqueueLog("RUDP BattleStart(roomId=" + battleStart.RoomId +
                           ", players=" + battleStart.PlayerASessionId +
                           "/" + battleStart.PlayerBSessionId + ")");
                return;
            }

            if (RudpPacketCodec.IsGameEvent(header))
            {
                if (!RudpPacketCodec.TryParseLootResolvedGameEventPayload(
                        payload,
                        payload.Length,
                        out RudpPacketCodec.RudpLootResolvedGameEventPayload lootResolved,
                        out error))
                {
                    EnqueueLog("RUDP GameEvent decode failed: " + error);
                    return;
                }

                ulong receivedCount = StoreLatestLootResolved(lootResolved);
                EnqueueLog("RUDP GameEvent.LootResolved(roomId=" + lootResolved.RoomId +
                           ", dropId=" + lootResolved.DropId +
                           ", winner=" + lootResolved.WinnerSessionId +
                           ", itemId=" + lootResolved.ItemId +
                           ", quantity=" + lootResolved.Quantity +
                           ", received=" + receivedCount + ")");
                return;
            }

            EnqueueLog("RUDP unsupported reliable packet(type=0x" + header.PacketType.ToString("X4") + ")");
        }

        private void SendAckOnly(
            UdpClient client,
            IPEndPoint remoteEndpoint,
            uint ack,
            uint ackBits)
        {
            try
            {
                uint sequence;
                lock (sendRoot)
                {
                    sequence = nextSequence;
                    byte[] packet = RudpPacketCodec.SerializeAckOnly(sequence, ack, ackBits);
                    client.Send(packet, packet.Length, remoteEndpoint);
                    AdvancePacketSequence();
                }
                EnqueueLog("RUDP ACK sent(ack=" + ack +
                           ", ackBits=0x" + ackBits.ToString("X8") +
                           ", sequence=" + sequence + ")");
            }
            catch (Exception ex) when (ex is SocketException || ex is ObjectDisposedException || ex is InvalidOperationException)
            {
                EnqueueLog("RUDP ACK failed: " + ex.Message);
            }
        }

        private bool TryStoreLatestStateSnapshot(
            RudpPacketCodec.RudpStateSnapshotPayload snapshot,
            out bool shouldLog,
            out ulong receivedCount)
        {
            lock (syncRoot)
            {
                shouldLog = false;
                receivedCount = stateSnapshotReceivedCount;
                if (hasLatestStateSnapshot &&
                    latestStateSnapshot.RoomId == snapshot.RoomId &&
                    snapshot.ServerTick <= latestStateSnapshot.ServerTick)
                {
                    return false;
                }

                bool firstSnapshot = !hasLatestStateSnapshot;
                bool roomChanged = hasLatestStateSnapshot && latestStateSnapshot.RoomId != snapshot.RoomId;
                DateTime now = DateTime.UtcNow;
                latestStateSnapshot = CopyStateSnapshot(snapshot);
                hasLatestStateSnapshot = true;
                latestStateSnapshotReceivedAtUtc = now;
                ++stateSnapshotReceivedCount;
                receivedCount = stateSnapshotReceivedCount;

                shouldLog =
                    firstSnapshot ||
                    roomChanged ||
                    (now - lastStateSnapshotLogAtUtc).TotalSeconds >= StateSnapshotLogIntervalSeconds;
                if (shouldLog)
                {
                    lastStateSnapshotLogAtUtc = now;
                    lastLoggedStateSnapshotTick = snapshot.ServerTick;
                }
                return true;
            }
        }

        private static RudpPacketCodec.RudpStateSnapshotPayload CopyStateSnapshot(
            RudpPacketCodec.RudpStateSnapshotPayload snapshot)
        {
            RudpPacketCodec.RudpStateSnapshotPlayer[] sourcePlayers =
                snapshot.Players ?? new RudpPacketCodec.RudpStateSnapshotPlayer[0];
            RudpPacketCodec.RudpStateSnapshotPlayer[] copiedPlayers =
                new RudpPacketCodec.RudpStateSnapshotPlayer[sourcePlayers.Length];
            Array.Copy(sourcePlayers, copiedPlayers, sourcePlayers.Length);
            return new RudpPacketCodec.RudpStateSnapshotPayload
            {
                RoomId = snapshot.RoomId,
                ServerTick = snapshot.ServerTick,
                Players = copiedPlayers
            };
        }

        private ulong StoreLatestLootResolved(RudpPacketCodec.RudpLootResolvedGameEventPayload payload)
        {
            lock (syncRoot)
            {
                latestLootResolved = payload;
                hasLatestLootResolved = true;
                ++lootResolvedReceivedCount;
                return lootResolvedReceivedCount;
            }
        }

        private ReceiveRecordResult RecordReliableSequence(uint sequence)
        {
            lock (syncRoot)
            {
                if (!hasReceiveAck)
                {
                    hasReceiveAck = true;
                    receiveAck = sequence;
                    receiveAckBits = 0;
                    return ReceiveRecordResult.Accepted;
                }

                if (sequence == receiveAck)
                {
                    return ReceiveRecordResult.Duplicate;
                }

                if (IsSequenceNewer(sequence, receiveAck))
                {
                    uint delta = sequence - receiveAck;
                    if (delta < 32)
                    {
                        receiveAckBits = (receiveAckBits << (int)delta) | (1U << ((int)delta - 1));
                    }
                    else if (delta == 32)
                    {
                        receiveAckBits = 1U << 31;
                    }
                    else
                    {
                        receiveAckBits = 0;
                    }
                    receiveAck = sequence;
                    return ReceiveRecordResult.Accepted;
                }

                uint distance = receiveAck - sequence;
                if (distance > 32)
                {
                    return ReceiveRecordResult.TooOld;
                }

                uint bit = 1U << ((int)distance - 1);
                if ((receiveAckBits & bit) != 0)
                {
                    return ReceiveRecordResult.Duplicate;
                }

                receiveAckBits |= bit;
                return ReceiveRecordResult.Accepted;
            }
        }

        private bool TryCurrentAck(out uint ack, out uint ackBits)
        {
            lock (syncRoot)
            {
                ack = receiveAck;
                ackBits = receiveAckBits;
                return hasReceiveAck;
            }
        }

        private void AdvancePacketSequence()
        {
            ++nextSequence;
            if (nextSequence == 0)
            {
                nextSequence = 1;
            }
        }

        private void AdvanceCommandSequence()
        {
            ++nextCommandSequence;
            if (nextCommandSequence == 0)
            {
                nextCommandSequence = 1;
            }
        }

        private void EnqueueLog(string line)
        {
            lock (syncRoot)
            {
                pendingLogLines.Enqueue(DateTime.Now.ToString("HH:mm:ss") + " " + line);
            }
        }

        private static bool IsSequenceNewer(uint candidate, uint baseline)
        {
            return unchecked((int)(candidate - baseline)) > 0;
        }

        private static IPEndPoint ReceiveAnyEndpoint(UdpClient client)
        {
            return client.Client.AddressFamily == AddressFamily.InterNetworkV6
                ? new IPEndPoint(IPAddress.IPv6Any, 0)
                : new IPEndPoint(IPAddress.Any, 0);
        }

        private static string LocalEndpointText(UdpClient client)
        {
            return client.Client.LocalEndPoint != null
                ? client.Client.LocalEndPoint.ToString()
                : "<unbound>";
        }

        private enum ReceiveRecordResult
        {
            Accepted,
            Duplicate,
            TooOld
        }

        public struct HelloHeartbeatStats
        {
            public bool Active;
            public ulong SentCount;
            public double LastAgeSeconds;
            public double NextDueSeconds;
        }

        public struct StateSnapshotStats
        {
            public bool HasSnapshot;
            public uint RoomId;
            public uint ServerTick;
            public int PlayerCount;
            public ulong ReceivedCount;
            public double AgeSeconds;
            public uint LastLoggedTick;
        }
    }
}
