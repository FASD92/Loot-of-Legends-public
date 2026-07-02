using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class LobbySceneControllerTests
    {
        [Test]
        public void InProgressRoomShowsGameInProgressAndCannotJoin()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();

                controller.ApplyServerRoomList(new[]
                {
                    new PlayerRoomListEntry(17U, 1, 2, PlayerRoomStatus.InProgress)
                });

                LobbySceneController.RoomRowView[] rows = controller.VisibleRoomRows;
                Assert.AreEqual(1, rows.Length);
                Assert.AreEqual("게임중", rows[0].StatusText);
                Assert.IsFalse(rows[0].JoinEnabled);
                Assert.IsFalse(controller.TryBuildJoinRoomRequest(0, out _));
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void FullRoomShowsFullAndCannotJoin()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();

                controller.ApplyServerRoomList(new[]
                {
                    new PlayerRoomListEntry(18U, 2, 2, PlayerRoomStatus.Open)
                });

                LobbySceneController.RoomRowView row = controller.VisibleRoomRows[0];
                Assert.AreEqual("가득 참", row.StatusText);
                Assert.IsFalse(row.JoinEnabled);
                Assert.IsFalse(controller.TryBuildJoinRoomRequest(0, out _));
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void OpenRoomCanJoinFromCurrentServerOriginRowOnly()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();
                PlayerRoomListEntry currentRoom =
                    new PlayerRoomListEntry(19U, 1, 4, PlayerRoomStatus.Open);

                controller.ApplyServerRoomList(new[] { currentRoom });

                Assert.IsTrue(controller.TryBuildJoinRoomRequest(0, out uint roomId));
                Assert.AreEqual(19U, roomId);
                Assert.IsTrue(controller.TryBuildJoinRoomRequest(currentRoom, out roomId));
                Assert.AreEqual(19U, roomId);
                Assert.IsFalse(controller.TryBuildJoinRoomRequest(
                    new PlayerRoomListEntry(77U, 0, 4, PlayerRoomStatus.Open),
                    out _));

                controller.ApplyPostReturnPending();

                Assert.IsFalse(controller.TryBuildJoinRoomRequest(currentRoom, out _));
                Assert.IsFalse(controller.TryBuildJoinRoomRequest(0, out _));
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void RoomRowsPreferServerTitleAndFallbackToRoomId()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();

                controller.ApplyServerRoomList(new[]
                {
                    new PlayerRoomListEntry(21U, 1, 4, PlayerRoomStatus.Open, "친구랑 한 판"),
                    new PlayerRoomListEntry(22U, 1, 4, PlayerRoomStatus.Open, string.Empty)
                });

                LobbySceneController.RoomRowView[] rows = controller.VisibleRoomRows;

                Assert.AreEqual("친구랑 한 판", rows[0].DisplayTitle);
                Assert.AreEqual("#22", rows[1].DisplayTitle);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void PostReturnPendingHidesStaleRowsAndDisablesCreate()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();
                controller.ApplyServerRoomList(new[]
                {
                    new PlayerRoomListEntry(20U, 1, 4, PlayerRoomStatus.Open)
                });

                controller.ApplyPostReturnPending();

                Assert.AreEqual(0, controller.VisibleRoomRows.Length);
                Assert.IsFalse(controller.CreateRoomEnabled);
                Assert.AreEqual("방 목록 갱신 후 이용할 수 있습니다", controller.StatusText);

                controller.AdvanceRoomListRefreshDelay(20.0f);

                Assert.IsFalse(controller.CreateRoomEnabled);
                Assert.AreEqual("방 목록 갱신 후 이용할 수 있습니다", controller.StatusText);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void DelayedWarningAppearsAfterThreeSecondsAndClearsAfterTenSeconds()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();

                controller.BeginRoomListRefresh();
                Assert.AreEqual("방 목록 갱신 중...", controller.StatusText);

                controller.AdvanceRoomListRefreshDelay(2.99f);
                Assert.AreEqual("방 목록 갱신 중...", controller.StatusText);

                controller.AdvanceRoomListRefreshDelay(0.01f);
                Assert.AreEqual("방 목록 갱신이 지연되고 있습니다", controller.StatusText);

                controller.AdvanceRoomListRefreshDelay(10.01f);
                Assert.AreEqual("방 목록 갱신 중...", controller.StatusText);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void CreateRoomDraftDefaultsToRelease0Capacity()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();

                Assert.AreEqual(PlayerTcpPacket.CreateRoomMaxCapacity, controller.CreateRoomDraftCapacity);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void CreateRoomDraftCapacityClampsToRelease0Range()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();

                controller.CreateRoomDraftCapacity = 1;
                Assert.AreEqual(PlayerTcpPacket.CreateRoomMinCapacity, controller.CreateRoomDraftCapacity);

                controller.CreateRoomDraftCapacity = 99;
                Assert.AreEqual(PlayerTcpPacket.CreateRoomMaxCapacity, controller.CreateRoomDraftCapacity);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void TryBuildCreateRoomRequestReturnsCurrentTitleAndCapacity()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();
                controller.CreateRoomDraftTitle = "Release 0";
                controller.CreateRoomDraftCapacity = 7;

                bool built = controller.TryBuildCreateRoomRequest(
                    out string roomTitle,
                    out int maxPlayers);

                Assert.IsTrue(built);
                Assert.AreEqual("Release 0", roomTitle);
                Assert.AreEqual(7, maxPlayers);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void TryBuildCreateRoomRequestAppendsActiveImeComposition()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();
                controller.CreateRoomDraftTitle = "hi my name is 최우";
                controller.CreateRoomDraftCapacity = 10;

                bool built = controller.TryBuildCreateRoomRequest(
                    "석",
                    out string roomTitle,
                    out int maxPlayers);

                Assert.IsTrue(built);
                Assert.AreEqual("hi my name is 최우석", roomTitle);
                Assert.AreEqual(10, maxPlayers);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void TryBuildCreateRoomRequestRejectsInvalidTitle()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();
                controller.CreateRoomDraftTitle = string.Empty;
                controller.CreateRoomDraftCapacity = 10;

                bool built = controller.TryBuildCreateRoomRequest(out _, out _);

                Assert.IsFalse(built);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task RequestCreateRoomAsyncSendsCurrentDraftTitleAndCapacity()
        {
            GameObject lobbyHost = new GameObject("LobbySceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                LobbySceneController lobby = lobbyHost.AddComponent<LobbySceneController>();
                PlayerNetworkSessionController network =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                FakeConnector connector = new FakeConnector { SessionIdToReturn = 11UL };
                FakeRudpSender rudpSender = new FakeRudpSender();
                PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
                await session.ConnectAsync(PlayerServerEndpoint.Default);
                connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
                connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                    17U,
                    PlayerRoomStatus.Open,
                    "보스방",
                    6,
                    PlayerTcpPacket.RoomActionLeaveRoom,
                    new PlayerRoomMemberEntry(11UL, "Player11", false)));
                SetSession(network, session);

                lobby.NetworkSessionController = network;
                lobby.CreateRoomDraftTitle = "보스방";
                lobby.CreateRoomDraftCapacity = 6;

                bool sent = await lobby.RequestCreateRoomAsync();

                Assert.IsTrue(sent);
                AssertCreateRoomPacket(connector.LastSentPacket, "보스방", 6);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(lobbyHost);
                UnityEngine.Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public async Task RunCreateRoomFlowAsyncRequestsRoomSceneAfterAcceptedCreate()
        {
            GameObject lobbyHost = new GameObject("LobbySceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                LobbySceneController lobby = lobbyHost.AddComponent<LobbySceneController>();
                PlayerNetworkSessionController network =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                FakeConnector connector = new FakeConnector { SessionIdToReturn = 11UL };
                FakeRudpSender rudpSender = new FakeRudpSender();
                PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
                await session.ConnectAsync(PlayerServerEndpoint.Default);
                connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
                connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                    17U,
                    PlayerRoomStatus.Open,
                    "보스방",
                    6,
                    PlayerTcpPacket.RoomActionLeaveRoom,
                    new PlayerRoomMemberEntry(11UL, "Player11", false)));
                SetSession(network, session);

                lobby.NetworkSessionController = network;
                lobby.CreateRoomDraftTitle = "보스방";
                lobby.CreateRoomDraftCapacity = 6;

                bool loaded = await lobby.RunCreateRoomFlowAsync();

                Assert.IsTrue(loaded);
                Assert.IsTrue(lobby.RoomSceneLoadRequested);
                Assert.AreEqual("방에 입장하고 있습니다", lobby.StatusText);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(lobbyHost);
                UnityEngine.Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public async Task RunCreateRoomFlowAsyncShowsFailureWhenCreateFails()
        {
            GameObject lobbyHost = new GameObject("LobbySceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                LobbySceneController lobby = lobbyHost.AddComponent<LobbySceneController>();
                PlayerNetworkSessionController network =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                FakeConnector connector = new FakeConnector { SessionIdToReturn = 11UL };
                FakeRudpSender rudpSender = new FakeRudpSender();
                PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
                await session.ConnectAsync(PlayerServerEndpoint.Default);
                SetSession(network, session);

                lobby.NetworkSessionController = network;
                lobby.CreateRoomDraftTitle = "보스방";
                lobby.CreateRoomDraftCapacity = 6;

                bool loaded = await lobby.RunCreateRoomFlowAsync();

                Assert.IsFalse(loaded);
                Assert.IsFalse(lobby.RoomSceneLoadRequested);
                Assert.AreEqual(
                    "방 요청에 실패했습니다. 다시 시도해주세요 (no packet queued)",
                    lobby.StatusText);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(lobbyHost);
                UnityEngine.Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public async Task UpdateDoesNotOverwriteVisibleRoomCommandFailureWithEmptyRoomList()
        {
            GameObject lobbyHost = new GameObject("LobbySceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                LobbySceneController lobby = lobbyHost.AddComponent<LobbySceneController>();
                PlayerNetworkSessionController network =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                FakeConnector connector = new FakeConnector { SessionIdToReturn = 11UL };
                FakeRudpSender rudpSender = new FakeRudpSender();
                PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
                await session.ConnectAsync(PlayerServerEndpoint.Default);
                SetSession(network, session);

                lobby.NetworkSessionController = network;
                lobby.CreateRoomDraftTitle = "보스방";
                lobby.CreateRoomDraftCapacity = 6;
                await lobby.RunCreateRoomFlowAsync();

                InvokeUpdate(lobby);

                Assert.AreEqual(
                    "방 요청에 실패했습니다. 다시 시도해주세요 (no packet queued)",
                    lobby.StatusText);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(lobbyHost);
                UnityEngine.Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public async Task RunJoinVisibleRoomFlowAsyncRequestsRoomSceneAfterAcceptedJoin()
        {
            GameObject lobbyHost = new GameObject("LobbySceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                LobbySceneController lobby = lobbyHost.AddComponent<LobbySceneController>();
                PlayerNetworkSessionController network =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                FakeConnector connector = new FakeConnector { SessionIdToReturn = 11UL };
                FakeRudpSender rudpSender = new FakeRudpSender();
                PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
                await session.ConnectAsync(PlayerServerEndpoint.Default);
                connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
                connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                    17U,
                    PlayerRoomStatus.Open,
                    "공개방",
                    6,
                    PlayerTcpPacket.RoomActionLeaveRoom,
                    new PlayerRoomMemberEntry(10UL, "Host10", false),
                    new PlayerRoomMemberEntry(11UL, "Player11", false)));
                SetSession(network, session);

                lobby.NetworkSessionController = network;
                lobby.ApplyServerRoomList(new[]
                {
                    new PlayerRoomListEntry(17U, 1, 6, PlayerRoomStatus.Open)
                });

                bool loaded = await lobby.RunJoinVisibleRoomFlowAsync(0);

                Assert.IsTrue(loaded);
                Assert.IsTrue(lobby.RoomSceneLoadRequested);
                Assert.AreEqual("방에 입장하고 있습니다", lobby.StatusText);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(lobbyHost);
                UnityEngine.Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public void DoesNotExposeRefreshOrPollRoomList()
        {
            GameObject host = new GameObject("LobbySceneController");
            try
            {
                LobbySceneController controller = host.AddComponent<LobbySceneController>();

                Assert.IsFalse(controller.RefreshButtonVisible);
                Assert.IsFalse(controller.PollsRoomList);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(host);
            }
        }

        private static void SetSession(
            PlayerNetworkSessionController controller,
            PlayerNetworkSession session)
        {
            FieldInfo field = typeof(PlayerNetworkSessionController).GetField(
                "session",
                BindingFlags.Instance | BindingFlags.NonPublic);
            field.SetValue(controller, session);
        }

        private static void InvokeUpdate(LobbySceneController controller)
        {
            typeof(LobbySceneController)
                .GetMethod("Update", BindingFlags.Instance | BindingFlags.NonPublic)
                .Invoke(controller, null);
        }

        private static void AssertCreateRoomPacket(
            byte[] packet,
            string expectedTitle,
            int expectedCapacity)
        {
            Assert.NotNull(packet);
            int size = (packet[0] << 8) | packet[1];
            int type = (packet[2] << 8) | packet[3];
            Assert.AreEqual(packet.Length, size);
            Assert.AreEqual(PlayerTcpPacket.CreateRoomRequestPacketType, type);
            int titleLength = packet[4];
            string title = Encoding.UTF8.GetString(packet, 5, titleLength);
            Assert.AreEqual(expectedTitle, title);
            Assert.AreEqual(expectedCapacity, packet[packet.Length - 1]);
        }

        private static byte[] CreateRoomResponsePacket(uint roomId, ushort playerCount)
        {
            byte[] packet = new byte[PlayerTcpPacket.CreateRoomResponsePacketSize];
            WriteU16BE(PlayerTcpPacket.CreateRoomResponsePacketSize, packet, 0);
            WriteU16BE(PlayerTcpPacket.CreateRoomResponsePacketType, packet, 2);
            WriteU32BE(roomId, packet, PlayerTcpPacket.HeaderSize);
            WriteU16BE(playerCount, packet, PlayerTcpPacket.HeaderSize + 4);
            return packet;
        }

        private static byte[] JoinRoomResponsePacket(uint roomId, ushort playerCount)
        {
            byte[] packet = new byte[PlayerTcpPacket.CreateRoomResponsePacketSize];
            WriteU16BE(PlayerTcpPacket.CreateRoomResponsePacketSize, packet, 0);
            WriteU16BE(PlayerTcpPacket.JoinRoomResponsePacketType, packet, 2);
            WriteU32BE(roomId, packet, PlayerTcpPacket.HeaderSize);
            WriteU16BE(playerCount, packet, PlayerTcpPacket.HeaderSize + 4);
            return packet;
        }

        private static byte[] RoomDetailStatePacket(
            uint roomId,
            PlayerRoomStatus status,
            string title,
            byte maxPlayers,
            ushort selfActionMask,
            params PlayerRoomMemberEntry[] members)
        {
            byte[] titleBytes = Encoding.UTF8.GetBytes(title);
            int size = 4 + 4 + 1 + 1 + titleBytes.Length + 1 + 1 + 2 + 1;
            foreach (PlayerRoomMemberEntry member in members)
            {
                size += 4 + 1 + Encoding.UTF8.GetByteCount(member.Nickname) + 1;
            }

            byte[] packet = new byte[size];
            WriteU16BE(size, packet, 0);
            WriteU16BE(PlayerTcpPacket.RoomDetailStatePacketType, packet, 2);
            int offset = 4;
            WriteU32BE(roomId, packet, offset);
            offset += 4;
            packet[offset++] = (byte)status;
            packet[offset++] = (byte)titleBytes.Length;
            Buffer.BlockCopy(titleBytes, 0, packet, offset, titleBytes.Length);
            offset += titleBytes.Length;
            packet[offset++] = maxPlayers;
            packet[offset++] = (byte)members.Length;
            foreach (PlayerRoomMemberEntry member in members)
            {
                byte[] nicknameBytes = Encoding.UTF8.GetBytes(member.Nickname);
                WriteU32BE((uint)member.SessionId, packet, offset);
                offset += 4;
                packet[offset++] = (byte)nicknameBytes.Length;
                Buffer.BlockCopy(nicknameBytes, 0, packet, offset, nicknameBytes.Length);
                offset += nicknameBytes.Length;
                packet[offset++] = member.Ready ? (byte)1 : (byte)0;
            }
            WriteU16BE(selfActionMask, packet, offset);
            offset += 2;
            packet[offset] = 0;
            return packet;
        }

        private static void WriteU16BE(int value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 8);
            packet[offset + 1] = (byte)value;
        }

        private static void WriteU32BE(uint value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 24);
            packet[offset + 1] = (byte)(value >> 16);
            packet[offset + 2] = (byte)(value >> 8);
            packet[offset + 3] = (byte)value;
        }

        private sealed class FakeConnector : IPlayerNetworkConnector
        {
            public int ConnectCallCount;
            public int SendPacketCallCount;
            public int ReceivePacketCallCount;
            public ulong SessionIdToReturn = 1UL;
            public byte[] LastSentPacket;
            public Queue<byte[]> ReceivePacketsToReturn = new Queue<byte[]>();

            public bool HasPendingPacket => ReceivePacketsToReturn.Count > 0;

            public Task<PlayerNetworkConnectResult> ConnectAsync(
                PlayerServerEndpoint endpoint,
                string gameSessionToken,
                CancellationToken cancellationToken)
            {
                ++ConnectCallCount;
                return Task.FromResult(new PlayerNetworkConnectResult(
                    SessionIdToReturn,
                    new PlayerClientListSnapshot(new[] { SessionIdToReturn })));
            }

            public Task SendPacketAsync(byte[] packet, CancellationToken cancellationToken)
            {
                ++SendPacketCallCount;
                LastSentPacket = packet;
                return Task.CompletedTask;
            }

            public Task<byte[]> ReceivePacketAsync(CancellationToken cancellationToken)
            {
                ++ReceivePacketCallCount;
                if (ReceivePacketsToReturn.Count == 0)
                {
                    throw new InvalidOperationException("no packet queued");
                }

                return Task.FromResult(ReceivePacketsToReturn.Dequeue());
            }

            public void Disconnect()
            {
            }

            public void Dispose()
            {
            }
        }

        private sealed class FakeRudpSender : IPlayerRudpSender
        {
            public Task<PlayerRudpHelloSendResult> SendHelloAsync(
                PlayerServerEndpoint endpoint,
                PlayerRudpHello hello,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(new PlayerRudpHelloSendResult(hello.SessionId, 1U, "127.0.0.1:40000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendAttackAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                uint targetHintMonsterId,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(new PlayerRudpInputCommandSendResult(sessionId, 1U, 1U, targetHintMonsterId, "127.0.0.1:40000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendMoveAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                short dirX,
                short dirY,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(new PlayerRudpInputCommandSendResult(sessionId, 1U, 1U, 0U, "127.0.0.1:40000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendSpaceLootAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(new PlayerRudpInputCommandSendResult(sessionId, 1U, 1U, 0U, "127.0.0.1:40000"));
            }

            public bool TryReceiveStateSnapshot(out PlayerRudpStateSnapshot snapshot)
            {
                snapshot = default;
                return false;
            }

            public void Disconnect()
            {
            }

            public void Dispose()
            {
            }
        }
    }
}
