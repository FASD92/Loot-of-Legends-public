using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using System.Reflection;
using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class RoomSceneControllerTests
    {
        [Test]
        public void RendersHostAsFirstServerOrderedMember()
        {
            GameObject host = new GameObject("RoomSceneController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();

                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(31UL, "민준", false),
                    new PlayerRoomMemberEntry(22UL, "서연", true)));

                Assert.AreEqual("방장: 민준", controller.HostText);
                Assert.AreEqual("방장", controller.VisibleMemberRows[0].RoleText);
                Assert.AreEqual(string.Empty, controller.VisibleMemberRows[1].RoleText);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void ShowsHostKickOnlyForServerProvidedTargetAction()
        {
            GameObject host = new GameObject("RoomSceneController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();

                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom |
                        PlayerTcpPacket.RoomActionHostStartBattle,
                    new[]
                    {
                        new PlayerRoomTargetActionEntry(33UL, PlayerTcpPacket.TargetActionHostKick)
                    },
                    new PlayerRoomMemberEntry(11UL, "host", false),
                    new PlayerRoomMemberEntry(22UL, "same-name", false),
                    new PlayerRoomMemberEntry(33UL, "same-name", false)));

                RoomSceneController.MemberRowView[] rows = controller.VisibleMemberRows;
                Assert.IsFalse(rows[0].KickVisible);
                Assert.IsFalse(rows[1].KickVisible);
                Assert.IsTrue(rows[2].KickVisible);
                Assert.AreEqual("강퇴", rows[2].KickButtonText);
                Assert.IsFalse(controller.TryBuildHostKickRequest(22UL, out _));
                Assert.IsTrue(controller.TryBuildHostKickRequest(33UL, out uint targetSessionId));
                Assert.AreEqual(33U, targetSessionId);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void SelfActionsComeOnlyFromServerActionMask()
        {
            GameObject host = new GameObject("RoomSceneController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();

                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(11UL, "host", true),
                    new PlayerRoomMemberEntry(22UL, "self", false)));

                Assert.IsTrue(controller.LeaveVisible);
                Assert.IsTrue(controller.LeaveRequestAvailable);
                Assert.IsTrue(controller.ReadyVisible);
                Assert.IsFalse(controller.UnreadyVisible);
                Assert.IsFalse(controller.HostStartBattleVisible);

                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionHostStartBattle,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(11UL, "host", false),
                    new PlayerRoomMemberEntry(22UL, "self", false)));

                Assert.IsFalse(controller.LeaveVisible);
                Assert.IsFalse(controller.LeaveRequestAvailable);
                Assert.IsFalse(controller.ReadyVisible);
                Assert.IsFalse(controller.UnreadyVisible);
                Assert.IsTrue(controller.HostStartBattleVisible);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void MemberOrderMatchesServerOrderAndReadyCopyIsKorean()
        {
            GameObject host = new GameObject("RoomSceneController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();

                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(44UL, "셋째", true),
                    new PlayerRoomMemberEntry(11UL, "첫째", false),
                    new PlayerRoomMemberEntry(22UL, "둘째", true)));

                RoomSceneController.MemberRowView[] rows = controller.VisibleMemberRows;
                Assert.AreEqual("셋째", rows[0].Nickname);
                Assert.AreEqual("첫째", rows[1].Nickname);
                Assert.AreEqual("둘째", rows[2].Nickname);
                Assert.AreEqual("준비 완료", rows[0].ReadyText);
                Assert.AreEqual("대기 중", rows[1].ReadyText);
                Assert.AreEqual("준비 완료", rows[2].ReadyText);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void CountCapacityTextUsesServerOriginMaxPlayersAndMemberCount()
        {
            GameObject host = new GameObject("RoomSceneController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();

                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(11UL, "host", false),
                    new PlayerRoomMemberEntry(22UL, "self", true)));

                Assert.AreEqual("현재 인원 2/4", controller.CountCapacityText);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void VisibleHeaderTextsIncludeCountCapacityText()
        {
            GameObject host = new GameObject("RoomSceneController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();

                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(11UL, "host", false),
                    new PlayerRoomMemberEntry(22UL, "self", true)));

                Assert.Contains("현재 인원 2/4", controller.VisibleHeaderTexts);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void CalculateLobbyReturnModalRectCentersWithinScaledGuiCoordinates()
        {
            Rect rect = RoomSceneController.CalculateLobbyReturnModalRect(2732, 1536);
            float scale = PlayerManualGuiScaler.CalculateScale(1536);

            float visualCenterX = (rect.x + rect.width * 0.5f) * scale;
            float visualCenterY = (rect.y + rect.height * 0.5f) * scale;

            Assert.AreEqual(2732.0f * 0.5f, visualCenterX, 0.5f);
            Assert.AreEqual(1536.0f * 0.5f, visualCenterY, 0.5f);
        }

        [Test]
        public void RequestArenaSceneLoadRequiresCurrentBattleIdentity()
        {
            GameObject host = new GameObject("RoomSceneController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();

                Assert.IsFalse(controller.RequestArenaSceneLoad(0U, 9001UL));
                Assert.IsFalse(controller.RequestArenaSceneLoad(17U, 0UL));
                Assert.IsFalse(controller.ArenaSceneLoadRequested);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void RequestArenaSceneLoadRecordsValidBattleEntry()
        {
            GameObject host = new GameObject("RoomSceneController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();

                bool requested = controller.RequestArenaSceneLoad(17U, 9001UL);

                Assert.IsTrue(requested);
                Assert.IsTrue(controller.ArenaSceneLoadRequested);
                Assert.AreEqual(17U, controller.ArenaSceneLoadRoomId);
                Assert.AreEqual(9001UL, controller.ArenaSceneLoadBattleInstanceId);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task RequestLeaveAsyncReturnsFalseWhenServerActionMaskDoesNotAllowLeave()
        {
            GameObject host = new GameObject("RoomSceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();
                controller.NetworkSessionController =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionReady,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(11UL, "host", false),
                    new PlayerRoomMemberEntry(22UL, "self", false)));

                bool sent = await controller.RequestLeaveAsync();

                Assert.IsFalse(sent);
            }
            finally
            {
                Object.DestroyImmediate(host);
                Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public async Task RequestLeaveAsyncRecordsLobbySceneLoadAfterAcceptedLeave()
        {
            GameObject host = new GameObject("RoomSceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();
                controller.NetworkSessionController =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                FakeConnector connector = new FakeConnector();
                connector.ReceivePacketsToReturn.Enqueue(LeaveRoomResponsePacket(17U));
                SetControllerSession(
                    controller.NetworkSessionController,
                    new PlayerNetworkSession(connector, new FakeRudpSender()));
                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(11UL, "host", false)));
                ApplyCapturedRoomDetail(
                    controller.NetworkSessionController,
                    DetailWithMembers(
                        PlayerTcpPacket.RoomActionLeaveRoom,
                        new PlayerRoomTargetActionEntry[0],
                        new PlayerRoomMemberEntry(11UL, "host", false)));

                bool left = await controller.RequestLeaveAsync();

                Assert.IsTrue(left);
                Assert.IsTrue(controller.LobbySceneLoadRequested);
                Assert.AreEqual(PlayerTcpPacket.SerializeLeaveRoomRequest(), connector.LastSentPacket);
                Assert.AreEqual(0U, controller.NetworkSessionController.CurrentRoomId);
            }
            finally
            {
                Object.DestroyImmediate(host);
                Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public void UpdateKeepsLastRoomViewButHidesActionsWhenConnectionIsLost()
        {
            GameObject host = new GameObject("RoomSceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();
                controller.NetworkSessionController =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                    new PlayerRoomTargetActionEntry[0],
                    new PlayerRoomMemberEntry(11UL, "host", false),
                    new PlayerRoomMemberEntry(22UL, "self", true)));

                InvokeUpdate(controller);

                Assert.AreEqual(2, controller.VisibleMemberRows.Length);
                Assert.IsFalse(controller.LeaveVisible);
                Assert.IsFalse(controller.ReadyVisible);
                Assert.AreEqual("현재 인원 2/4", controller.CountCapacityText);
                Assert.AreEqual(RoomSceneController.ConnectionLostMessage, controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(host);
                Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public void UpdateShowsWaitingStateWhenRoomDetailWasNeverCaptured()
        {
            GameObject host = new GameObject("RoomSceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();
                controller.NetworkSessionController =
                    networkHost.AddComponent<PlayerNetworkSessionController>();

                InvokeUpdate(controller);

                Assert.AreEqual(0, controller.VisibleMemberRows.Length);
                Assert.IsFalse(controller.LeaveVisible);
                Assert.IsFalse(controller.ReadyVisible);
                Assert.AreEqual("현재 인원 0/0", controller.CountCapacityText);
                Assert.AreEqual(RoomSceneController.RoomInfoWaitingMessage, controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(host);
                Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public void UpdateShowsHostKickLobbyReturnModalFromServerReason()
        {
            GameObject host = new GameObject("RoomSceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();
                controller.NetworkSessionController =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                controller.ApplyRoomDetailState(DetailWithMembers(
                    PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                    new[]
                    {
                        new PlayerRoomTargetActionEntry(22UL, PlayerTcpPacket.TargetActionHostKick)
                    },
                    new PlayerRoomMemberEntry(11UL, "host", false),
                    new PlayerRoomMemberEntry(22UL, "target", true)));
                ApplyLobbyReturn(
                    controller.NetworkSessionController,
                    PlayerLobbyReturnReason.HostKick);

                InvokeUpdate(controller);

                Assert.IsTrue(controller.LobbyReturnModalVisible);
                Assert.AreEqual(
                    RoomSceneController.HostKickReturnMessage,
                    controller.LobbyReturnModalMessage);
                Assert.AreEqual(
                    RoomSceneController.HostKickReturnMessage,
                    controller.StatusText);
                Assert.AreEqual(0, controller.VisibleMemberRows.Length);
                Assert.IsFalse(controller.LeaveVisible);
                Assert.IsFalse(controller.ReadyVisible);
                Assert.IsFalse(controller.UnreadyVisible);
                Assert.IsFalse(controller.HostStartBattleVisible);
                Assert.AreEqual("현재 인원 0/0", controller.CountCapacityText);
                Assert.AreNotEqual(
                    RoomSceneController.RoomInfoWaitingMessage,
                    controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(host);
                Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public void UpdateDoesNotShowHostKickModalForNonHostKickLobbyReturn()
        {
            GameObject host = new GameObject("RoomSceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();
                controller.NetworkSessionController =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                ApplyLobbyReturn(
                    controller.NetworkSessionController,
                    PlayerLobbyReturnReason.None);

                InvokeUpdate(controller);

                Assert.IsFalse(controller.LobbyReturnModalVisible);
                Assert.AreEqual(string.Empty, controller.LobbyReturnModalMessage);
                Assert.AreEqual(RoomSceneController.RoomInfoWaitingMessage, controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(host);
                Object.DestroyImmediate(networkHost);
            }
        }

        [Test]
        public void UpdatePrefersFreshRoomDetailOverStaleHostKickLobbyReturn()
        {
            GameObject host = new GameObject("RoomSceneController");
            GameObject networkHost = new GameObject("NetworkSessionController");
            try
            {
                RoomSceneController controller = host.AddComponent<RoomSceneController>();
                controller.NetworkSessionController =
                    networkHost.AddComponent<PlayerNetworkSessionController>();
                ApplyLobbyReturn(
                    controller.NetworkSessionController,
                    PlayerLobbyReturnReason.HostKick);
                ApplyCapturedRoomDetail(
                    controller.NetworkSessionController,
                    DetailWithMembers(
                        PlayerTcpPacket.RoomActionLeaveRoom,
                        new PlayerRoomTargetActionEntry[0],
                        new PlayerRoomMemberEntry(41UL, "new-host", false),
                        new PlayerRoomMemberEntry(42UL, "self", false)));

                InvokeUpdate(controller);

                Assert.IsFalse(controller.LobbyReturnModalVisible);
                Assert.AreEqual(string.Empty, controller.LobbyReturnModalMessage);
                Assert.AreEqual("방장: new-host", controller.HostText);
                Assert.AreEqual(2, controller.VisibleMemberRows.Length);
                Assert.AreEqual(string.Empty, controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(host);
                Object.DestroyImmediate(networkHost);
            }
        }

        private static void InvokeUpdate(RoomSceneController controller)
        {
            typeof(RoomSceneController)
                .GetMethod("Update", BindingFlags.Instance | BindingFlags.NonPublic)
                .Invoke(controller, null);
        }

        private static void ApplyLobbyReturn(
            PlayerNetworkSessionController controller,
            PlayerLobbyReturnReason reason)
        {
            PlayerNetworkSession session = controller.Session;
            SetSessionField(session, "status", PlayerNetworkSessionStatus.Connected);
            SetSessionField(session, "currentRoomId", 0U);
            SetSessionField(session, "roomDetailCaptured", false);
            SetSessionField(session, "lobbyReturnCaptured", true);
            SetSessionField(session, "lobbyReturnPreviousRoomId", 17U);
            SetSessionField(session, "lobbyReturnReason", reason);
        }

        private static void ApplyCapturedRoomDetail(
            PlayerNetworkSessionController controller,
            PlayerRoomDetailState detail)
        {
            PlayerNetworkSession session = controller.Session;
            SetSessionField(session, "status", PlayerNetworkSessionStatus.Connected);
            SetSessionField(session, "currentRoomId", detail.RoomId);
            SetSessionField(session, "currentRoomPlayerCount", (ushort)detail.MemberCount);
            SetSessionField(session, "roomDetailCaptured", true);
            SetSessionField(session, "roomDetailState", detail);
        }

        private static void SetControllerSession(
            PlayerNetworkSessionController controller,
            PlayerNetworkSession session)
        {
            FieldInfo field = typeof(PlayerNetworkSessionController).GetField(
                "session",
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.NotNull(field, "session");
            field.SetValue(controller, session);
        }

        private static void SetSessionField<T>(
            PlayerNetworkSession session,
            string fieldName,
            T value)
        {
            FieldInfo field = typeof(PlayerNetworkSession).GetField(
                fieldName,
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.NotNull(field, fieldName);
            field.SetValue(session, value);
        }

        private static PlayerRoomDetailState DetailWithMembers(
            ushort selfActionMask,
            PlayerRoomTargetActionEntry[] targetActions,
            params PlayerRoomMemberEntry[] members)
        {
            return new PlayerRoomDetailState(
                17U,
                PlayerRoomStatus.Open,
                "Release 0",
                4,
                members,
                selfActionMask,
                targetActions);
        }

        private static byte[] LeaveRoomResponsePacket(uint roomId)
        {
            byte[] packet = new byte[PlayerTcpPacket.RoomIdPacketSize];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x06;
            WriteU32BE(roomId, packet, PlayerTcpPacket.HeaderSize);
            return packet;
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
            public readonly Queue<byte[]> ReceivePacketsToReturn = new Queue<byte[]>();
            public byte[] LastSentPacket;

            public bool HasPendingPacket => ReceivePacketsToReturn.Count > 0;

            public Task<PlayerNetworkConnectResult> ConnectAsync(
                PlayerServerEndpoint endpoint,
                string gameSessionToken,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(new PlayerNetworkConnectResult(
                    11UL,
                    new PlayerClientListSnapshot(new[] { 11UL }),
                    new PlayerRoomListSnapshot(new PlayerRoomListEntry[0])));
            }

            public Task SendPacketAsync(byte[] packet, CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                LastSentPacket = packet;
                return Task.CompletedTask;
            }

            public Task<byte[]> ReceivePacketAsync(CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (ReceivePacketsToReturn.Count == 0)
                {
                    throw new System.InvalidOperationException("no packet queued");
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
                throw new System.NotSupportedException();
            }

            public Task<PlayerRudpInputCommandSendResult> SendAttackAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                uint targetHintMonsterId,
                CancellationToken cancellationToken)
            {
                throw new System.NotSupportedException();
            }

            public Task<PlayerRudpInputCommandSendResult> SendMoveAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                short dirX,
                short dirY,
                CancellationToken cancellationToken)
            {
                throw new System.NotSupportedException();
            }

            public Task<PlayerRudpInputCommandSendResult> SendSpaceLootAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                CancellationToken cancellationToken)
            {
                throw new System.NotSupportedException();
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
