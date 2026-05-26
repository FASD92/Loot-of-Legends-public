using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.ThinClient
{
    internal sealed class TcpDebugSession : IDisposable
    {
        private readonly object syncRoot = new object();
        private readonly object sendRoot = new object();
        private readonly List<byte> receiveBuffer = new List<byte>(2048);
        private readonly Queue<string> pendingLogLines = new Queue<string>();
        private readonly TcpDebugClientState state = new TcpDebugClientState();

        private TcpClient client;
        private NetworkStream stream;
        private CancellationTokenSource cancellation;
        private bool connected;

        public TcpDebugSession(string alias)
        {
            Alias = alias;
        }

        public string Alias { get; private set; }

        public bool IsConnected
        {
            get
            {
                lock (syncRoot)
                {
                    return connected;
                }
            }
        }

        public ulong SessionId
        {
            get
            {
                lock (syncRoot)
                {
                    return state.HasSessionId ? state.SessionId : 0;
                }
            }
        }

        public uint RoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return state.HasRoomId ? state.RoomId : 0;
                }
            }
        }

        public TcpDropSourceObservation DropSource
        {
            get
            {
                lock (syncRoot)
                {
                    return state.DropSource;
                }
            }
        }

        public TcpLootResolvedObservation LatestLootResolved
        {
            get
            {
                lock (syncRoot)
                {
                    return state.LatestLootResolved;
                }
            }
        }

        public TcpLootRejectedObservation LatestLootRejected
        {
            get
            {
                lock (syncRoot)
                {
                    return state.LatestLootRejected;
                }
            }
        }

        public TcpInventorySnapshotObservation LatestInventorySnapshot
        {
            get
            {
                lock (syncRoot)
                {
                    return state.LatestInventorySnapshot;
                }
            }
        }

        public void Connect(string host, ushort port)
        {
            Disconnect();

            CancellationTokenSource nextCancellation = new CancellationTokenSource();
            lock (syncRoot)
            {
                cancellation = nextCancellation;
                state.Reset();
                receiveBuffer.Clear();
                EnqueueLogLocked(Alias + " connecting to " + host + ":" + port);
            }

            _ = Task.Run(() => RunReceiveLoop(host, port, nextCancellation.Token));
        }

        public void Disconnect()
        {
            CancellationTokenSource cancellationToStop = null;
            TcpClient clientToClose = null;

            lock (syncRoot)
            {
                cancellationToStop = cancellation;
                cancellation = null;
                clientToClose = client;
                client = null;
                stream = null;
                connected = false;
                receiveBuffer.Clear();
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

        public void SendCreateRoom()
        {
            SendPacket(TcpPacketCodec.SerializeCreateRoomRequest(), "CreateRoomRequest");
        }

        public void SendJoinRoom(uint roomId)
        {
            if (roomId == 0)
            {
                EnqueueLog(Alias + " cannot send JoinRoomRequest without a confirmed roomId");
                return;
            }

            SendPacket(TcpPacketCodec.SerializeJoinRoomRequest(roomId), "JoinRoomRequest(roomId=" + roomId + ")");
        }

        public void SendReady()
        {
            SendPacket(TcpPacketCodec.SerializeReadyRoomRequest(), "ReadyRoomRequest");
        }

        public void SendClickLootRequest(uint dropId)
        {
            if (dropId == 0)
            {
                EnqueueLog(Alias + " cannot send ClickLootRequest without a server-origin dropId");
                return;
            }

            SendPacket(TcpPacketCodec.SerializeClickLootRequest(dropId), "ClickLootRequest(dropId=" + dropId + ")");
        }

        public void SendSmokeCreateCenterDropRequest()
        {
            SendPacket(TcpPacketCodec.SerializeSmokeCreateCenterDropRequest(), "SmokeCreateCenterDropRequest");
        }

        public void SendSmokePlacePlayersAroundCenterDropRequest()
        {
            SendPacket(TcpPacketCodec.SerializeSmokePlacePlayersAroundCenterDropRequest(), "SmokePlacePlayersAroundCenterDropRequest");
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

        public void Dispose()
        {
            Disconnect();
        }

        private void RunReceiveLoop(string host, ushort port, CancellationToken token)
        {
            TcpClient localClient = new TcpClient();
            try
            {
                localClient.NoDelay = true;
                localClient.Connect(host, port);
                NetworkStream localStream = localClient.GetStream();

                lock (syncRoot)
                {
                    if (token.IsCancellationRequested)
                    {
                        return;
                    }
                    client = localClient;
                    stream = localStream;
                    connected = true;
                    EnqueueLogLocked(Alias + " connected");
                }

                byte[] chunk = new byte[4096];
                while (!token.IsCancellationRequested)
                {
                    int bytesRead = localStream.Read(chunk, 0, chunk.Length);
                    if (bytesRead == 0)
                    {
                        EnqueueLog(Alias + " remote closed connection");
                        break;
                    }

                    bool keepReading;
                    lock (syncRoot)
                    {
                        for (int i = 0; i < bytesRead; ++i)
                        {
                            receiveBuffer.Add(chunk[i]);
                        }
                        keepReading = DrainReceiveBufferLocked();
                    }

                    if (!keepReading)
                    {
                        break;
                    }
                }
            }
            catch (ObjectDisposedException)
            {
                if (!token.IsCancellationRequested)
                {
                    EnqueueLog(Alias + " socket disposed");
                }
            }
            catch (IOException ex)
            {
                if (!token.IsCancellationRequested)
                {
                    EnqueueLog(Alias + " receive failed: " + ex.Message);
                }
            }
            catch (SocketException ex)
            {
                if (!token.IsCancellationRequested)
                {
                    EnqueueLog(Alias + " socket failed: " + ex.Message);
                }
            }
            finally
            {
                lock (syncRoot)
                {
                    if (client == localClient)
                    {
                        client = null;
                        stream = null;
                        connected = false;
                        receiveBuffer.Clear();
                    }
                }
                localClient.Close();
            }
        }

        private bool DrainReceiveBufferLocked()
        {
            while (receiveBuffer.Count >= TcpPacketCodec.HeaderSize)
            {
                byte[] headerBytes = new byte[TcpPacketCodec.HeaderSize];
                for (int i = 0; i < TcpPacketCodec.HeaderSize; ++i)
                {
                    headerBytes[i] = receiveBuffer[i];
                }

                if (!TcpPacketCodec.TryPeekHeader(
                        headerBytes,
                        headerBytes.Length,
                        out int packetSize,
                        out TcpPacketType packetType,
                        out ushort rawType,
                        out string error))
                {
                    EnqueueLogLocked(Alias + " invalid TCP header: " + error);
                    return false;
                }

                if (receiveBuffer.Count < packetSize)
                {
                    return true;
                }

                byte[] packet = receiveBuffer.GetRange(0, packetSize).ToArray();
                receiveBuffer.RemoveRange(0, packetSize);

                if (TcpPacketCodec.TryDescribePacket(Alias, state, packet, out string description, out error))
                {
                    EnqueueLogLocked(description);
                }
                else
                {
                    EnqueueLogLocked(Alias + " failed to decode " + packetType + "(0x" + rawType.ToString("X4") + "): " + error);
                    return false;
                }
            }

            return true;
        }

        private void SendPacket(byte[] packet, string label)
        {
            NetworkStream localStream;
            lock (syncRoot)
            {
                if (!connected || stream == null)
                {
                    EnqueueLogLocked(Alias + " cannot send " + label + ": not connected");
                    return;
                }

                localStream = stream;
            }

            try
            {
                lock (sendRoot)
                {
                    localStream.Write(packet, 0, packet.Length);
                    localStream.Flush();
                }
                EnqueueLog(Alias + " sent " + label);
            }
            catch (Exception ex) when (ex is IOException || ex is ObjectDisposedException || ex is SocketException)
            {
                EnqueueLog(Alias + " send failed: " + ex.Message);
                Disconnect();
            }
        }

        private void EnqueueLog(string line)
        {
            lock (syncRoot)
            {
                EnqueueLogLocked(line);
            }
        }

        private void EnqueueLogLocked(string line)
        {
            pendingLogLines.Enqueue(DateTime.Now.ToString("HH:mm:ss") + " " + line);
        }
    }
}
