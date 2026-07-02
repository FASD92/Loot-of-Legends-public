using System;
using System.Collections.Generic;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerNetworkSessionTests
    {
        [Test]
        public async Task ConnectAsyncUsesConnectorAndMarksConnected()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(PlayerServerEndpoint.Default.DisplayName, session.Endpoint.DisplayName);
            Assert.AreEqual(1, connector.ConnectCallCount);
            Assert.AreEqual(PlayerServerEndpoint.Default.DisplayName, connector.LastEndpoint.DisplayName);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task ConnectAsyncStoresWelcomeSessionId()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 0x0102030405060708UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0x0102030405060708UL, session.SessionId);
        }

        [Test]
        public async Task ConnectAsyncPassesGameSessionTokenToConnector()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default, "dev-session:player1");

            Assert.AreEqual("dev-session:player1", connector.LastGameSessionToken);
        }

        [Test]
        public async Task ConnectAsyncStoresClientListSnapshot()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL,
                SnapshotSessionIdsToReturn = new[] { 11UL, 22UL, 33UL }
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(3, session.ClientSessionCount);
            Assert.IsTrue(session.SelfListedInClientListSnapshot);
            Assert.AreEqual(new[] { 11UL, 22UL, 33UL }, session.ClientSessionIds);
        }

        [Test]
        public async Task ConnectAsyncStoresInitialRoomListSnapshot()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL,
                RoomListEntriesToReturn = new[]
                {
                    new PlayerRoomListEntry(17U, 1, 10)
                }
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, session.RoomListCount);
            Assert.AreEqual(17U, session.RoomListEntries[0].RoomId);
        }

        [Test]
        public async Task ConnectAsyncSendsRudpHelloForCapturedSessionBeforeConnected()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 0x0102030405060708UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, rudpSender.SendHelloCallCount);
            Assert.AreEqual(PlayerServerEndpoint.Default.DisplayName, rudpSender.LastEndpoint.DisplayName);
            Assert.AreEqual(1, rudpSender.LastHello.ClientVersion);
            Assert.AreEqual(0x05060708U, rudpSender.LastHello.ClientId);
            Assert.AreEqual(0x0102030405060708UL, rudpSender.LastHello.SessionId);
            Assert.IsTrue(session.RudpHelloSent);
            Assert.AreEqual(0x0102030405060708UL, session.RudpHelloSessionId);
            Assert.AreEqual(1U, session.RudpHelloSequence);
        }

        [Test]
        public async Task ConnectAsyncFailureMarksFailedAndKeepsError()
        {
            FakeConnector connector = new FakeConnector
            {
                ExceptionToThrow = new InvalidOperationException("connection refused")
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("connection refused", session.LastError);
        }

        [Test]
        public async Task ConnectAsyncFailureBeforeWelcomeDoesNotSendRudpHello()
        {
            FakeConnector connector = new FakeConnector
            {
                ExceptionToThrow = new InvalidOperationException("connection refused")
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual(0, rudpSender.SendHelloCallCount);
            Assert.IsFalse(session.RudpHelloSent);
        }

        [Test]
        public async Task ConnectAsyncRudpHelloFailureMarksFailedAndClearsSession()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 77UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender
            {
                ExceptionToThrow = new InvalidOperationException("udp failed")
            };
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual(0UL, session.SessionId);
            Assert.IsFalse(session.RudpHelloSent);
            Assert.AreEqual("udp failed", session.LastError);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task DisconnectCancelsConnectorAndMarksDisconnected()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 12UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            session.Disconnect();

            Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, session.Status);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task DisconnectClearsCapturedSessionId()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 77UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            session.Disconnect();

            Assert.AreEqual(0UL, session.SessionId);
            Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, session.Status);
        }

        [Test]
        public async Task DisconnectClearsClientListSnapshot()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL,
                SnapshotSessionIdsToReturn = new[] { 11UL, 22UL }
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            session.Disconnect();

            Assert.AreEqual(0, session.ClientSessionCount);
            Assert.IsFalse(session.SelfListedInClientListSnapshot);
            Assert.AreEqual(new ulong[0], session.ClientSessionIds);
        }

        [Test]
        public async Task DisconnectClearsRudpHelloStateAndClosesSender()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 77UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            session.Disconnect();

            Assert.IsFalse(session.RudpHelloSent);
            Assert.AreEqual(0UL, session.RudpHelloSessionId);
            Assert.AreEqual(0U, session.RudpHelloSequence);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task StaleConnectFailureDoesNotDisconnectCurrentSession()
        {
            TaskCompletionSource<PlayerNetworkConnectResult> firstAttempt =
                new TaskCompletionSource<PlayerNetworkConnectResult>();
            TaskCompletionSource<PlayerNetworkConnectResult> secondAttempt =
                new TaskCompletionSource<PlayerNetworkConnectResult>();
            FakeConnector connector = new FakeConnector();
            connector.ConnectHandler = () =>
                connector.ConnectCallCount == 1 ? firstAttempt.Task : secondAttempt.Task;
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            Task firstConnect = session.ConnectAsync(PlayerServerEndpoint.Default);
            Task secondConnect = session.ConnectAsync(PlayerServerEndpoint.Default);

            secondAttempt.SetResult(new PlayerNetworkConnectResult(200UL));
            await secondConnect;
            firstAttempt.SetException(new InvalidOperationException("stale failure"));
            await firstConnect;

            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(1, connector.DisconnectCallCount);
        }

        [Test]
        public async Task StaleConnectSuccessDoesNotOverwriteCurrentSessionId()
        {
            TaskCompletionSource<PlayerNetworkConnectResult> firstAttempt =
                new TaskCompletionSource<PlayerNetworkConnectResult>();
            TaskCompletionSource<PlayerNetworkConnectResult> secondAttempt =
                new TaskCompletionSource<PlayerNetworkConnectResult>();
            FakeConnector connector = new FakeConnector();
            connector.ConnectHandler = () =>
                connector.ConnectCallCount == 1 ? firstAttempt.Task : secondAttempt.Task;
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            Task firstConnect = session.ConnectAsync(PlayerServerEndpoint.Default);
            Task secondConnect = session.ConnectAsync(PlayerServerEndpoint.Default);

            secondAttempt.SetResult(new PlayerNetworkConnectResult(200UL));
            await secondConnect;
            firstAttempt.SetResult(new PlayerNetworkConnectResult(100UL));
            await firstConnect;

            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(200UL, session.SessionId);
        }

        [Test]
        public async Task StaleConnectSuccessDoesNotSendRudpHelloForStaleAttempt()
        {
            TaskCompletionSource<PlayerNetworkConnectResult> firstAttempt =
                new TaskCompletionSource<PlayerNetworkConnectResult>();
            TaskCompletionSource<PlayerNetworkConnectResult> secondAttempt =
                new TaskCompletionSource<PlayerNetworkConnectResult>();
            FakeConnector connector = new FakeConnector();
            connector.ConnectHandler = () =>
                connector.ConnectCallCount == 1 ? firstAttempt.Task : secondAttempt.Task;
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            Task firstConnect = session.ConnectAsync(PlayerServerEndpoint.Default);
            Task secondConnect = session.ConnectAsync(PlayerServerEndpoint.Default);

            secondAttempt.SetResult(new PlayerNetworkConnectResult(200UL));
            await secondConnect;
            firstAttempt.SetResult(new PlayerNetworkConnectResult(100UL));
            await firstConnect;

            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, rudpSender.SendHelloCallCount);
            Assert.AreEqual(200UL, rudpSender.LastHello.SessionId);
            Assert.AreEqual(200UL, session.RudpHelloSessionId);
        }

        [Test]
        public async Task SendCreateRoomRequestAsyncSendsPacketOnlyWhenConnected()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 44UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool sent = await session.SendCreateRoomRequestAsync();

            Assert.IsTrue(sent);
            Assert.AreEqual(1, connector.SendPacketCallCount);
            Assert.AreEqual(PlayerTcpPacket.SerializeCreateRoomRequest(), connector.LastSentPacket);
        }

        [Test]
        public async Task SendHeartbeatAsyncSendsPacketOnlyWhenConnected()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 44UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool sent = await session.SendHeartbeatAsync();

            Assert.IsTrue(sent);
            Assert.AreEqual(1, connector.SendPacketCallCount);
            Assert.AreEqual(PlayerTcpPacket.SerializeHeartbeatRequest(), connector.LastSentPacket);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
        }

        [Test]
        public async Task SendHeartbeatAsyncReturnsFalseWhenDisconnected()
        {
            FakeConnector connector = new FakeConnector();
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            bool sent = await session.SendHeartbeatAsync();

            Assert.IsFalse(sent);
            Assert.AreEqual(0, connector.SendPacketCallCount);
            Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, session.Status);
        }

        [Test]
        public async Task SendCreateRoomRequestAsyncPassesTitleAndCapacity()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 44UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool sent = await session.SendCreateRoomRequestAsync("abc", 10);

            Assert.IsTrue(sent);
            Assert.AreEqual(1, connector.SendPacketCallCount);
            Assert.AreEqual(
                PlayerTcpPacket.SerializeCreateRoomRequest("abc", 10),
                connector.LastSentPacket);
        }

        [Test]
        public async Task SendCreateRoomRequestAsyncReturnsFalseWhenDisconnected()
        {
            FakeConnector connector = new FakeConnector();
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);

            bool sent = await session.SendCreateRoomRequestAsync();

            Assert.IsFalse(sent);
            Assert.AreEqual(0, connector.SendPacketCallCount);
            Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, session.Status);
        }

        [Test]
        public async Task SendCreateRoomRequestAsyncFailureMarksFailedAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 55UL,
                SendExceptionToThrow = new InvalidOperationException("write failed")
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool sent = await session.SendCreateRoomRequestAsync();

            Assert.IsFalse(sent);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("write failed", session.LastError);
            Assert.AreEqual(0UL, session.SessionId);
            Assert.AreEqual(0, session.ClientSessionCount);
            Assert.IsFalse(session.RudpHelloSent);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestCreateRoomAsyncCapturesResponseAndRoomList()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(17U, 1, 10),
                new PlayerRoomListEntry(21U, 2, 10)));
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new PlayerRoomMemberEntry(11UL, "Player11", false)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestCreateRoomAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, connector.SendPacketCallCount);
            Assert.AreEqual(3, connector.ReceivePacketCallCount);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(1, session.CurrentRoomPlayerCount);
            Assert.AreEqual(2, session.RoomListCount);
            Assert.AreEqual(17U, session.RoomListEntries[0].RoomId);
            Assert.AreEqual(10, session.RoomListEntries[0].MaxPlayers);
            Assert.AreEqual(21U, session.RoomListEntries[1].RoomId);
            Assert.IsTrue(session.RoomDetailCaptured);
            Assert.AreEqual("Room17", session.RoomTitle);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestCreateRoomAsyncCapturesRoomDetailState()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new PlayerRoomMemberEntry(11UL, "Player11", false)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestCreateRoomAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(1, session.CurrentRoomPlayerCount);
            Assert.IsTrue(session.RoomDetailCaptured);
            Assert.AreEqual("Room17", session.RoomTitle);
            Assert.AreEqual(PlayerRoomStatus.Open, session.CurrentRoomStatus);
            Assert.AreEqual(1, session.RoomMembers.Length);
            Assert.AreEqual(11UL, session.RoomMembers[0].SessionId);
            Assert.AreEqual(PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady, session.SelfRoomActionMask);
            Assert.AreEqual(0, session.RoomListCount);
        }

        [Test]
        public async Task CaptureRoomListSnapshotAsyncCapturesRoomListWithoutSendingCommand()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(17U, 1, 2)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.CaptureRoomListSnapshotAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0, connector.SendPacketCallCount);
            Assert.AreEqual(1, connector.ReceivePacketCallCount);
            Assert.AreEqual(1, session.RoomListCount);
            Assert.AreEqual(17U, session.RoomListEntries[0].RoomId);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestCreateRoomAsyncSkipsClientListSnapshotBeforeResponse()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(ClientListSnapshotPacket(11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestCreateRoomAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, connector.SendPacketCallCount);
            Assert.AreEqual(3, connector.ReceivePacketCallCount);
            Assert.AreEqual(2, session.ClientSessionCount);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.IsTrue(session.RoomDetailCaptured);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestCreateRoomAsyncKeepsResponseIfBackgroundDrainReadsItFirst()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            Assert.IsFalse(await session.DrainIncomingTcpStateAsync());
            bool captured = await session.RequestCreateRoomAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, connector.SendPacketCallCount);
            Assert.AreEqual(2, connector.ReceivePacketCallCount);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(1, session.CurrentRoomPlayerCount);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.IsTrue(session.RoomDetailCaptured);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestCreateRoomAsyncKeepsConnectionOnCreateRoomError()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(ErrorPacket(
                PlayerTcpPacket.CreateRoomRequestPacketType,
                PlayerTcpPacket.ErrorCodeAlreadyInRoom));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestCreateRoomAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(11UL, session.SessionId);
            Assert.IsTrue(session.RudpHelloSent);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual("create room rejected: AlreadyInRoom", session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestCreateRoomAsyncRejectsInvalidResponseAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestCreateRoomAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid create room response packet", session.LastError);
            Assert.AreEqual(0UL, session.SessionId);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestCreateRoomAsyncRejectsRoomListOnlyWithoutRoomDetailState()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(17U, 1, 10),
                new PlayerRoomListEntry(21U, 1, 10)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestCreateRoomAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("no packet queued", session.LastError);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestJoinRoomAsyncCapturesResponseAndRoomList()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(17U, 2, 10),
                new PlayerRoomListEntry(21U, 1, 10)));
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", false),
                    new PlayerRoomMemberEntry(22UL, "Player22", false)
                },
                new PlayerRoomTargetActionEntry[0]));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestJoinRoomAsync(17U);

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, connector.SendPacketCallCount);
            Assert.AreEqual(PlayerTcpPacket.SerializeJoinRoomRequest(17U), connector.LastSentPacket);
            Assert.AreEqual(3, connector.ReceivePacketCallCount);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(2, session.CurrentRoomPlayerCount);
            Assert.AreEqual(2, session.RoomListCount);
            Assert.AreEqual(17U, session.RoomListEntries[0].RoomId);
            Assert.AreEqual(2, session.RoomListEntries[0].PlayerCount);
            Assert.IsTrue(session.RoomDetailCaptured);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestJoinRoomAsyncCapturesRoomDetailState()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", false),
                    new PlayerRoomMemberEntry(22UL, "Player22", false)
                },
                new PlayerRoomTargetActionEntry[0]));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestJoinRoomAsync(17U);

            Assert.IsTrue(captured);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(2, session.CurrentRoomPlayerCount);
            Assert.IsTrue(session.RoomDetailCaptured);
            Assert.AreEqual(2, session.RoomMembers.Length);
            Assert.AreEqual(11UL, session.RoomMembers[0].SessionId);
            Assert.AreEqual(22UL, session.RoomMembers[1].SessionId);
            Assert.AreEqual(0, session.RoomListCount);
        }

        [Test]
        public async Task RequestJoinRoomAsyncSkipsClientListSnapshotBeforeResponse()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(ClientListSnapshotPacket(11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestJoinRoomAsync(17U);

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, connector.SendPacketCallCount);
            Assert.AreEqual(3, connector.ReceivePacketCallCount);
            Assert.AreEqual(2, session.ClientSessionCount);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(2, session.CurrentRoomPlayerCount);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.IsTrue(session.RoomDetailCaptured);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestJoinRoomAsyncRejectsRoomListOnlyWithoutRoomDetailState()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(17U, 2, 10)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestJoinRoomAsync(17U);

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("no packet queued", session.LastError);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestJoinRoomAsyncRejectsZeroRoomIdWithoutSending()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestJoinRoomAsync(0U);

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0, connector.SendPacketCallCount);
            Assert.AreEqual(0, connector.ReceivePacketCallCount);
            Assert.AreEqual("invalid room id", session.LastError);
        }

        [Test]
        public async Task RequestJoinRoomAsyncKeepsConnectionOnJoinRoomError()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(ErrorPacket(
                PlayerTcpPacket.JoinRoomRequestPacketType,
                PlayerTcpPacket.ErrorCodeAlreadyStarted));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestJoinRoomAsync(17U);

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(22UL, session.SessionId);
            Assert.IsTrue(session.RudpHelloSent);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual("join room rejected: AlreadyStarted", session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestJoinRoomAsyncRejectsMismatchedResponseAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(99U, 2));
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(99U, 2, 10)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestJoinRoomAsync(17U);

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid join room response packet", session.LastError);
            Assert.AreEqual(0UL, session.SessionId);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestJoinRoomAsyncRejectsRoomListWithoutRoomDetailEvenWhenJoinedRoomMissing()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(21U, 1, 10)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestJoinRoomAsync(17U);

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("no packet queued", session.LastError);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestLeaveRoomAsyncSendsLeaveRequestAndReturnsToLobbyRoomList()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new PlayerRoomMemberEntry(11UL, "Player11", false)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestCreateRoomAsync();
            connector.ReceivePacketsToReturn.Enqueue(LeaveRoomResponsePacket(17U));
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(17U, 0, 10)));

            bool left = await session.RequestLeaveRoomAsync();

            Assert.IsTrue(left);
            Assert.AreEqual(PlayerTcpPacket.SerializeLeaveRoomRequest(), connector.LastSentPacket);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.CurrentRoomPlayerCount);
            Assert.IsFalse(session.RoomDetailCaptured);
            Assert.AreEqual(1, session.RoomListCount);
            Assert.AreEqual(17U, session.RoomListEntries[0].RoomId);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestLeaveRoomAsyncDropsStaleOldRoomDetailBeforeLobbyRoomList()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new PlayerRoomMemberEntry(11UL, "Player11", false)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestCreateRoomAsync();
            connector.ReceivePacketsToReturn.Enqueue(LeaveRoomResponsePacket(17U));
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new PlayerRoomMemberEntry(11UL, "Player11", false)));
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(21U, 1, 10)));

            bool left = await session.RequestLeaveRoomAsync();

            Assert.IsTrue(left);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.IsFalse(session.RoomDetailCaptured);
            Assert.AreEqual(1, session.RoomListCount);
            Assert.AreEqual(21U, session.RoomListEntries[0].RoomId);
            Assert.AreEqual(5, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task RequestLeaveRoomAsyncRejectsNoCurrentRoomWithoutSending()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool left = await session.RequestLeaveRoomAsync();

            Assert.IsFalse(left);
            Assert.AreEqual(0, connector.SendPacketCallCount);
            Assert.AreEqual("not in room", session.LastError);
        }

        [Test]
        public async Task RequestReadyRoomAsyncCapturesReadyStatus()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 1, 2));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(2, connector.SendPacketCallCount);
            Assert.AreEqual(PlayerTcpPacket.SerializeReadyRoomRequest(), connector.LastSentPacket);
            Assert.AreEqual(3, connector.ReceivePacketCallCount);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(1, session.CurrentRoomReadyPlayerCount);
            Assert.AreEqual(2, session.CurrentRoomPlayerCount);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestReadyRoomAsyncAppliesPendingRoomDetailState()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", false),
                    new PlayerRoomMemberEntry(22UL, "Player22", false)
                },
                new PlayerRoomTargetActionEntry[0]));
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 1, 2));
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionUnready,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", false),
                    new PlayerRoomMemberEntry(22UL, "Player22", true)
                },
                new PlayerRoomTargetActionEntry[0]));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(1, session.CurrentRoomReadyPlayerCount);
            Assert.AreEqual(2, session.CurrentRoomPlayerCount);
            Assert.IsTrue(session.RoomMembers[1].Ready);
            Assert.AreEqual(PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionUnready, session.SelfRoomActionMask);
        }

        [Test]
        public async Task RequestHostStartBattleAsyncSendsRequestAndCapturesResponse()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionHostStartBattle,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", true),
                    new PlayerRoomMemberEntry(22UL, "Player22", true)
                },
                new[]
                {
                    new PlayerRoomTargetActionEntry(22UL, PlayerTcpPacket.TargetActionHostKick)
                }));
            connector.ReceivePacketsToReturn.Enqueue(RoomIdPacket(
                PlayerTcpPacket.HostStartBattleResponsePacketType,
                17U));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestCreateRoomAsync();

            bool captured = await session.RequestHostStartBattleAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerTcpPacket.SerializeHostStartBattleRequest(), connector.LastSentPacket);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestReadyRoomAsyncSkipsRoomListSnapshotBeforeResponse()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                new PlayerRoomListEntry(17U, 2, 10)));
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 1, 2));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestCreateRoomAsync();

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(2, connector.SendPacketCallCount);
            Assert.AreEqual(4, connector.ReceivePacketCallCount);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(1, session.CurrentRoomReadyPlayerCount);
            Assert.AreEqual(2, session.CurrentRoomPlayerCount);
            Assert.AreEqual(1, session.RoomListCount);
            Assert.AreEqual(2, session.RoomListEntries[0].PlayerCount);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task RequestReadyRoomAsyncHandlesSessionReplacedBeforeResponse()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(SessionReplacedPacket());
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestCreateRoomAsync();

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, session.Status);
            Assert.AreEqual(PlayerNetworkSession.SessionReplacedMessage, session.LastError);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestReadyRoomAsyncAppliesBattlePacketsBeforeResponse()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 7U, 100U, 30));
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestCreateRoomAsync();

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(2, connector.SendPacketCallCount);
            Assert.AreEqual(5, connector.ReceivePacketCallCount);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(2, session.CurrentRoomReadyPlayerCount);
            Assert.AreEqual(2, session.CurrentRoomPlayerCount);
            Assert.IsTrue(session.BattleStarted);
            Assert.AreEqual(17U, session.BattleStartRoomId);
            Assert.IsTrue(session.MonsterSpawned);
            Assert.AreEqual(7U, session.MonsterId);
            Assert.AreEqual(string.Empty, session.LastError);

            Assert.IsTrue(await session.CaptureBattleStartAsync());
            Assert.IsTrue(await session.CaptureMonsterSpawnAsync());
            Assert.AreEqual(5, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task DrainIncomingTcpStateAsyncAppliesBattleMonsterAndHealthPackets()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterHealthSnapshotPacket(17U, 1001U, 75, 100));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());

            Assert.IsTrue(session.BattleStarted);
            Assert.IsTrue(session.MonsterSpawned);
            Assert.IsTrue(session.MonsterHealthSnapshotCaptured);
            Assert.AreEqual(1001U, session.MonsterId);
            Assert.AreEqual(75, session.MonsterCurrentHp);
            Assert.AreEqual(100, session.MonsterHealthMaxHp);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(5, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task DrainIncomingTcpStateAsyncConsumesRoomDetailStateWithoutLocalRoomContext()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new PlayerRoomMemberEntry(22UL, "Player22", false)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool applied = await session.DrainIncomingTcpStateAsync();

            Assert.IsTrue(applied);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.CurrentRoomPlayerCount);
            Assert.IsFalse(session.RoomDetailCaptured);
            Assert.AreEqual(string.Empty, session.RoomTitle);
            Assert.AreEqual(1, connector.ReceivePacketCallCount);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task DrainIncomingTcpStateAsyncDefersRoomDetailWhileRoomVisibilityCommandWaits()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom,
                new PlayerRoomMemberEntry(22UL, "Player22", false)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            SetRoomVisibilityCommandInFlight(session, true);

            bool applied = await session.DrainIncomingTcpStateAsync();

            Assert.IsFalse(applied);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.IsFalse(session.RoomDetailCaptured);
            Assert.AreEqual(1, connector.ReceivePacketCallCount);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task DrainIncomingTcpStateAsyncAppliesMonsterDeathAndDrops()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterHealthSnapshotPacket(17U, 1001U, 0, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, -2500, 3000)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());

            Assert.IsTrue(session.MonsterDeathCaptured);
            Assert.AreEqual(1001U, session.MonsterDeathMonsterId);
            Assert.IsTrue(session.DropListSnapshotV2Captured);
            Assert.AreEqual(1, session.DropListSnapshotV2DropCount);
            Assert.AreEqual(1U, session.DropListSnapshotV2Drops[0].DropId);
            Assert.AreEqual(-2500, session.DropListSnapshotV2Drops[0].PosX);
            Assert.AreEqual(3000, session.DropListSnapshotV2Drops[0].PosY);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(7, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task DrainIncomingTcpStateAsyncAppliesBroadcastLootResolvedWithoutLocalSpaceLoot()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterHealthSnapshotPacket(17U, 1001U, 0, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, -2500, 3000)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 11UL, 1001U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());

            bool applied = await session.DrainIncomingTcpStateAsync();

            Assert.IsTrue(applied);
            Assert.IsTrue(session.LootResolvedCaptured);
            Assert.AreEqual(17U, session.LootResolved.RoomId);
            Assert.AreEqual(1U, session.LootResolved.DropId);
            Assert.AreEqual(11UL, session.LootResolved.WinnerSessionId);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task DrainIncomingTcpStateAsyncAppliesLootResolvedAndInventorySnapshot()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 22UL, 1001U, 1));
            connector.ReceivePacketsToReturn.Enqueue(InventorySnapshotPacket(
                22UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 1)));

            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());
            Assert.IsTrue(await session.DrainIncomingTcpStateAsync());

            Assert.IsTrue(session.LootResolvedCaptured);
            Assert.AreEqual(1U, session.LootResolved.DropId);
            Assert.IsTrue(session.InventorySnapshotCaptured);
            Assert.AreEqual(22UL, session.InventorySnapshot.SessionId);
            Assert.AreEqual(1001U, session.InventorySnapshot.EntryAt(0).ItemId);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.IsTrue(await session.CaptureLootResolvedAsync());
            Assert.IsTrue(await session.CaptureInventorySnapshotAsync());
            Assert.AreEqual(9, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task RequestReadyRoomAsyncRejectsNoCurrentRoomWithoutSending()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0, connector.SendPacketCallCount);
            Assert.AreEqual(0, connector.ReceivePacketCallCount);
            Assert.AreEqual("not in room", session.LastError);
        }

        [Test]
        public async Task RequestReadyRoomAsyncKeepsConnectionOnReadyRoomError()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ErrorPacket(
                PlayerTcpPacket.ReadyRoomRequestPacketType,
                PlayerTcpPacket.ErrorCodeNotInRoom));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(22UL, session.SessionId);
            Assert.IsTrue(session.RudpHelloSent);
            Assert.AreEqual(17U, session.CurrentRoomId);
            Assert.AreEqual(0, session.CurrentRoomReadyPlayerCount);
            Assert.AreEqual(2, session.CurrentRoomPlayerCount);
            Assert.AreEqual("ready room rejected: NotInRoom", session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestReadyRoomAsyncRejectsMismatchedResponseAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(99U, 1, 2));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid ready room response packet", session.LastError);
            Assert.AreEqual(0UL, session.SessionId);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.CurrentRoomReadyPlayerCount);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task RequestReadyRoomAsyncRejectsImpossibleReadyCounts()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 3, 2));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool captured = await session.RequestReadyRoomAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid ready room response packet", session.LastError);
            Assert.AreEqual(0, session.CurrentRoomReadyPlayerCount);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task CaptureBattleStartAsyncCapturesBattleState()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();

            bool captured = await session.CaptureBattleStartAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.BattleStarted);
            Assert.AreEqual(17U, session.BattleStartRoomId);
            Assert.AreEqual(11UL, session.BattleStartPlayerASessionId);
            Assert.AreEqual(22UL, session.BattleStartPlayerBSessionId);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task CaptureBattleStartAsyncRejectsNoCurrentRoomWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.CaptureBattleStartAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0, connector.ReceivePacketCallCount);
            Assert.AreEqual("not in room", session.LastError);
        }

        [Test]
        public async Task CaptureBattleStartAsyncRejectsMismatchedRoomAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(99U, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool captured = await session.CaptureBattleStartAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid battle start packet", session.LastError);
            Assert.IsFalse(session.BattleStarted);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task CaptureBattleStartAsyncRejectsBattleStartWithoutSelfAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 33UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool captured = await session.CaptureBattleStartAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid battle start packet", session.LastError);
            Assert.IsFalse(session.BattleStarted);
        }

        [Test]
        public async Task CaptureBattleLoadEntryAsyncStoresBattleInstanceWithoutGameplayStart()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.CaptureBattleStartAsync();

            bool captured = await session.CaptureBattleLoadEntryAsync();

            Assert.IsTrue(captured);
            Assert.IsTrue(session.BattleLoadEntryCaptured);
            Assert.AreEqual(17U, session.BattleLoadEntryRoomId);
            Assert.AreEqual(9001UL, session.BattleInstanceId);
            CollectionAssert.AreEqual(new[] { 11UL, 22UL }, session.BattleLoadPlayerSessionIds);
            Assert.IsFalse(session.ArenaLoadCompleteSent);
            Assert.IsFalse(session.ArenaGameplayStarted);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task SendArenaLoadCompleteAsyncUsesCapturedRoomAndBattleInstance()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();

            bool sent = await session.SendArenaLoadCompleteAsync();

            Assert.IsTrue(sent);
            Assert.IsTrue(session.ArenaLoadCompleteSent);
            CollectionAssert.AreEqual(
                PlayerTcpPacket.SerializeArenaLoadComplete(17U, 9001UL),
                connector.LastSentPacket);
        }

        [Test]
        public async Task CaptureArenaGameplayStartAsyncEnablesGameplayForMatchingBattle()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();

            bool captured = await session.CaptureArenaGameplayStartAsync();

            Assert.IsTrue(captured);
            Assert.IsTrue(session.ArenaGameplayStarted);
            Assert.AreEqual(17U, session.ArenaGameplayStartRoomId);
            Assert.AreEqual(9001UL, session.ArenaGameplayStartBattleInstanceId);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task CaptureArenaGameplayStartAsyncReturnsCachedStateAfterFrameDrain()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();

            TaskCompletionSource<byte[]> frameDrainReceive = new TaskCompletionSource<byte[]>();
            TaskCompletionSource<byte[]> captureReceive = new TaskCompletionSource<byte[]>();
            int receiveCallCount = 0;
            connector.ReceivePacketsToReturn.Enqueue(new byte[] { 0xFF });
            connector.ReceiveHandler = _ =>
            {
                ++receiveCallCount;
                return receiveCallCount == 1 ?
                    frameDrainReceive.Task :
                    captureReceive.Task;
            };

            Task<bool> frameDrainTask = session.DrainIncomingTcpStateAsync();
            Task<bool> captureTask = session.CaptureArenaGameplayStartAsync();
            frameDrainReceive.SetResult(ArenaGameplayStartPacket(17U, 9001UL));

            try
            {
                Assert.IsTrue(await frameDrainTask, session.LastError);
                Task completedTask = await Task.WhenAny(captureTask, Task.Delay(100));

                Assert.AreSame(
                    captureTask,
                    completedTask,
                    "arena gameplay start capture should return from cached state after frame drain");
                Assert.IsTrue(await captureTask, session.LastError);
                Assert.IsTrue(session.ArenaGameplayStarted);
            }
            finally
            {
                if (!captureTask.IsCompleted)
                {
                    captureReceive.SetResult(ArenaGameplayStartPacket(17U, 9001UL));
                    await captureTask;
                }
            }
        }

        [Test]
        public async Task CaptureArenaGameplayStartAsyncRejectsMismatchedBattleAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9002UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();

            bool captured = await session.CaptureArenaGameplayStartAsync();

            Assert.IsFalse(captured);
            Assert.IsFalse(session.ArenaGameplayStarted);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid arena gameplay start packet", session.LastError);
        }

        [Test]
        public async Task CaptureMonsterSpawnAsyncRejectsBeforeArenaGameplayStartWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 30));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();

            bool captured = await session.CaptureMonsterSpawnAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsFalse(session.MonsterSpawned);
            Assert.AreEqual("arena gameplay not started", session.LastError);
            Assert.AreEqual(4, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureMonsterSpawnAsyncCapturesMonsterState()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 30));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();

            bool captured = await session.CaptureMonsterSpawnAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.MonsterSpawned);
            Assert.AreEqual(17U, session.MonsterSpawnRoomId);
            Assert.AreEqual(1001U, session.MonsterId);
            Assert.AreEqual(2001U, session.MonsterTypeId);
            Assert.AreEqual(30, session.MonsterMaxHp);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(5, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureMonsterSpawnAsyncRejectsNoCurrentRoomWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);

            bool captured = await session.CaptureMonsterSpawnAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0, connector.ReceivePacketCallCount);
            Assert.AreEqual("not in room", session.LastError);
        }

        [Test]
        public async Task CaptureMonsterSpawnAsyncRejectsBeforeBattleStartWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool captured = await session.CaptureMonsterSpawnAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(2, connector.ReceivePacketCallCount);
            Assert.AreEqual("battle not started", session.LastError);
        }

        [Test]
        public async Task CaptureMonsterSpawnAsyncRejectsMismatchedRoomAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(99U, 1001U, 2001U, 30));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();

            bool captured = await session.CaptureMonsterSpawnAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid monster spawn packet", session.LastError);
            Assert.IsFalse(session.MonsterSpawned);
            Assert.AreEqual(0U, session.MonsterSpawnRoomId);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task CaptureMonsterSpawnAsyncRejectsInvalidMonsterIdentityAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 0U, 2001U, 30));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();

            bool captured = await session.CaptureMonsterSpawnAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid monster spawn packet", session.LastError);
            Assert.IsFalse(session.MonsterSpawned);
            Assert.AreEqual(0U, session.MonsterId);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task SendAttackIntentAsyncSendsCapturedMonsterIdAfterMonsterSpawn()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 30));
            FakeRudpSender rudpSender = new FakeRudpSender
            {
                AttackSequenceToReturn = 2U,
                AttackCommandSequenceToReturn = 1U
            };
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();

            bool sent = await session.SendAttackIntentAsync();

            Assert.IsTrue(sent);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, rudpSender.SendAttackCallCount);
            Assert.AreEqual(PlayerServerEndpoint.Default.DisplayName, rudpSender.LastEndpoint.DisplayName);
            Assert.AreEqual(22UL, rudpSender.LastAttackSessionId);
            Assert.AreEqual(1001U, rudpSender.LastAttackTargetHintMonsterId);
            Assert.IsTrue(session.RudpAttackSent);
            Assert.AreEqual(22UL, session.RudpAttackSessionId);
            Assert.AreEqual(2U, session.RudpAttackSequence);
            Assert.AreEqual(1U, session.RudpAttackCommandSequence);
            Assert.AreEqual(1001U, session.RudpAttackTargetHintMonsterId);
            Assert.AreEqual("127.0.0.1:50000", session.RudpAttackLocalEndpoint);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task SendMoveIntentAsyncSendsScaledDirectionAfterRoomJoin()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            FakeRudpSender rudpSender = new FakeRudpSender
            {
                MoveSequenceToReturn = 4U,
                MoveCommandSequenceToReturn = 3U
            };
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool sent = await session.SendMoveIntentAsync(
                PlayerInputIntent.FromKeyboard(1.0f, -1.0f, false));

            Assert.IsTrue(sent);
            Assert.AreEqual(1, rudpSender.SendMoveCallCount);
            Assert.AreEqual(22UL, rudpSender.LastMoveSessionId);
            Assert.AreEqual(1000, rudpSender.LastMoveDirX);
            Assert.AreEqual(-1000, rudpSender.LastMoveDirY);
            Assert.IsTrue(session.RudpMoveSent);
            Assert.AreEqual(22UL, session.RudpMoveSessionId);
            Assert.AreEqual(4U, session.RudpMoveSequence);
            Assert.AreEqual(3U, session.RudpMoveCommandSequence);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task DrainIncomingRudpStateSnapshotAppliesCurrentRoomSnapshot()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            FakeRudpSender rudpSender = new FakeRudpSender
            {
                StateSnapshotToReturn = new PlayerRudpStateSnapshot(
                    17U,
                    7U,
                    new[]
                    {
                        new PlayerRudpStateSnapshotPlayer(11UL, -1000, 0),
                        new PlayerRudpStateSnapshotPlayer(22UL, 1000, 100)
                    })
            };
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);

            bool applied = session.DrainIncomingRudpStateSnapshot();

            Assert.IsTrue(applied);
            Assert.IsTrue(session.StateSnapshotCaptured);
            Assert.AreEqual(17U, session.StateSnapshot.RoomId);
            Assert.AreEqual(7U, session.StateSnapshot.ServerTick);
            Assert.AreEqual(2, session.StateSnapshot.Count);
            Assert.AreEqual(11UL, session.StateSnapshot.Players[0].SessionId);
            Assert.AreEqual(-1.0f, session.StateSnapshot.Players[0].WorldX, 0.0001f);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task SendAttackIntentAsyncRejectsBeforeMonsterSpawnWithoutSending()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();

            bool sent = await session.SendAttackIntentAsync();

            Assert.IsFalse(sent);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0, rudpSender.SendAttackCallCount);
            Assert.AreEqual("monster not spawned", session.LastError);
        }

        [Test]
        public async Task SendAttackIntentAsyncFailureMarksFailedAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 30));
            FakeRudpSender rudpSender = new FakeRudpSender
            {
                AttackExceptionToThrow = new InvalidOperationException("udp attack failed")
            };
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();

            bool sent = await session.SendAttackIntentAsync();

            Assert.IsFalse(sent);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("udp attack failed", session.LastError);
            Assert.AreEqual(0UL, session.SessionId);
            Assert.IsFalse(session.RudpHelloSent);
            Assert.IsFalse(session.RudpAttackSent);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task CaptureMonsterHealthSnapshotAsyncCapturesHealthAfterMonsterSpawn()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterHealthSnapshotPacket(17U, 1001U, 75, 100));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();

            bool captured = await session.CaptureMonsterHealthSnapshotAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.MonsterHealthSnapshotCaptured);
            Assert.AreEqual(17U, session.MonsterHealthRoomId);
            Assert.AreEqual(1001U, session.MonsterHealthMonsterId);
            Assert.AreEqual(75, session.MonsterCurrentHp);
            Assert.AreEqual(100, session.MonsterHealthMaxHp);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(6, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureMonsterHealthSnapshotAsyncReturnsAlreadyCapturedWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterHealthSnapshotPacket(17U, 1001U, 75, 100));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterHealthSnapshotAsync();
            int receiveCountAfterFirstCapture = connector.ReceivePacketCallCount;

            bool captured = await session.CaptureMonsterHealthSnapshotAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(receiveCountAfterFirstCapture, connector.ReceivePacketCallCount);
            Assert.AreEqual(75, session.MonsterCurrentHp);
            Assert.AreEqual(100, session.MonsterHealthMaxHp);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task CaptureMonsterHealthSnapshotAsyncRejectsBeforeMonsterSpawnWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();

            bool captured = await session.CaptureMonsterHealthSnapshotAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(4, connector.ReceivePacketCallCount);
            Assert.AreEqual("monster not spawned", session.LastError);
        }

        [Test]
        public async Task CaptureMonsterHealthSnapshotAsyncRejectsMismatchedMonsterAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterHealthSnapshotPacket(17U, 9999U, 75, 100));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();

            bool captured = await session.CaptureMonsterHealthSnapshotAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid monster health snapshot packet", session.LastError);
            Assert.IsFalse(session.MonsterHealthSnapshotCaptured);
            Assert.AreEqual(0U, session.MonsterCurrentHp);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task CaptureMonsterDeathAsyncCapturesDeathAfterMonsterSpawn()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();

            bool captured = await session.CaptureMonsterDeathAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.MonsterDeathCaptured);
            Assert.AreEqual(17U, session.MonsterDeathRoomId);
            Assert.AreEqual(1001U, session.MonsterDeathMonsterId);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(6, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureMonsterDeathAsyncRejectsBeforeMonsterSpawnWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();

            bool captured = await session.CaptureMonsterDeathAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(4, connector.ReceivePacketCallCount);
            Assert.AreEqual("monster not spawned", session.LastError);
        }

        [Test]
        public async Task CaptureMonsterDeathAsyncRejectsMismatchedMonsterAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 9999U));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();

            bool captured = await session.CaptureMonsterDeathAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid monster death packet", session.LastError);
            Assert.IsFalse(session.MonsterDeathCaptured);
            Assert.AreEqual(0U, session.MonsterDeathMonsterId);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task CaptureDropListSnapshotV2AsyncCapturesDropsAfterMonsterDeath()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, -2500, 3000),
                new PlayerDropEntryV2(2U, 1002U, 3, 0, -1250)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();

            bool captured = await session.CaptureDropListSnapshotV2Async();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.DropListSnapshotV2Captured);
            Assert.AreEqual(17U, session.DropListSnapshotV2RoomId);
            Assert.AreEqual(12345U, session.DropListSnapshotV2ScatterSeed);
            Assert.AreEqual(2, session.DropListSnapshotV2DropCount);
            Assert.AreEqual(1U, session.DropListSnapshotV2Drops[0].DropId);
            Assert.AreEqual(1001U, session.DropListSnapshotV2Drops[0].ItemId);
            Assert.AreEqual(1, session.DropListSnapshotV2Drops[0].Quantity);
            Assert.AreEqual(-2500, session.DropListSnapshotV2Drops[0].PosX);
            Assert.AreEqual(3000, session.DropListSnapshotV2Drops[0].PosY);
            Assert.AreEqual(2U, session.DropListSnapshotV2Drops[1].DropId);
            Assert.AreEqual(-1250, session.DropListSnapshotV2Drops[1].PosY);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(7, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureDropListSnapshotV2AsyncRejectsBeforeMonsterDeathWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();

            bool captured = await session.CaptureDropListSnapshotV2Async();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(5, connector.ReceivePacketCallCount);
            Assert.AreEqual("monster death not captured", session.LastError);
        }

        [Test]
        public async Task CaptureDropListSnapshotV2AsyncRejectsMismatchedRoomAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                99U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();

            bool captured = await session.CaptureDropListSnapshotV2Async();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid drop list snapshot v2 packet", session.LastError);
            Assert.IsFalse(session.DropListSnapshotV2Captured);
            Assert.AreEqual(0, session.DropListSnapshotV2DropCount);
            Assert.AreEqual(new PlayerDropEntryV2[0], session.DropListSnapshotV2Drops);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task SendSpaceLootIntentAsyncSendsAfterDropListSnapshotV2()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            FakeRudpSender rudpSender = new FakeRudpSender
            {
                SpaceLootSequenceToReturn = 3U,
                SpaceLootCommandSequenceToReturn = 2U
            };
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();

            bool sent = await session.SendSpaceLootIntentAsync();

            Assert.IsTrue(sent);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(1, rudpSender.SendSpaceLootCallCount);
            Assert.AreEqual(PlayerServerEndpoint.Default.DisplayName, rudpSender.LastEndpoint.DisplayName);
            Assert.AreEqual(22UL, rudpSender.LastSpaceLootSessionId);
            Assert.IsTrue(session.RudpSpaceLootSent);
            Assert.AreEqual(22UL, session.RudpSpaceLootSessionId);
            Assert.AreEqual(3U, session.RudpSpaceLootSequence);
            Assert.AreEqual(2U, session.RudpSpaceLootCommandSequence);
            Assert.AreEqual("127.0.0.1:50000", session.RudpSpaceLootLocalEndpoint);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task CaptureLootResolvedAsyncCapturesAfterSpaceLootSend()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 22UL, 1001U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();

            bool captured = await session.CaptureLootResolvedAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.LootResolvedCaptured);
            Assert.AreEqual(17U, session.LootResolved.RoomId);
            Assert.AreEqual(1U, session.LootResolved.DropId);
            Assert.AreEqual(22UL, session.LootResolved.WinnerSessionId);
            Assert.AreEqual(1001U, session.LootResolved.ItemId);
            Assert.AreEqual(1, session.LootResolved.Quantity);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(8, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureLootRejectedAsyncCapturesAfterSpaceLootSend()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootRejectedPacket(17U, 1U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();

            bool captured = await session.CaptureLootRejectedAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.LootRejectedCaptured);
            Assert.AreEqual(17U, session.LootRejected.RoomId);
            Assert.AreEqual(1U, session.LootRejected.DropId);
            Assert.AreEqual(PlayerLootRejectReason.AlreadyClaimed, session.LootRejected.Reason);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(8, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureLootRejectedAsyncSkipsBroadcastLootResolvedBeforeRejected()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 11UL, 1001U, 1));
            connector.ReceivePacketsToReturn.Enqueue(LootRejectedPacket(17U, 1U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();

            bool captured = await session.CaptureLootRejectedAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.LootResolvedCaptured);
            Assert.AreEqual(11UL, session.LootResolved.WinnerSessionId);
            Assert.IsTrue(session.LootRejectedCaptured);
            Assert.AreEqual(17U, session.LootRejected.RoomId);
            Assert.AreEqual(1U, session.LootRejected.DropId);
            Assert.AreEqual(PlayerLootRejectReason.AlreadyClaimed, session.LootRejected.Reason);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
            Assert.AreEqual(9, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureLootRejectedAsyncAcceptsRejectedAfterDropRemovalSnapshot()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 11UL, 1001U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(17U, 12346U));
            connector.ReceivePacketsToReturn.Enqueue(LootRejectedPacket(17U, 1U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();

            bool captured = await session.CaptureLootRejectedAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.LootResolvedCaptured);
            Assert.AreEqual(11UL, session.LootResolved.WinnerSessionId);
            Assert.AreEqual(0, session.DropListSnapshotV2DropCount);
            Assert.IsTrue(session.LootRejectedCaptured);
            Assert.AreEqual(17U, session.LootRejected.RoomId);
            Assert.AreEqual(1U, session.LootRejected.DropId);
            Assert.AreEqual(PlayerLootRejectReason.AlreadyClaimed, session.LootRejected.Reason);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
            Assert.AreEqual(10, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureLootRejectedAsyncAcceptsResolvedAfterDropRemovalSnapshot()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(17U, 12346U));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 11UL, 1001U, 1));
            connector.ReceivePacketsToReturn.Enqueue(LootRejectedPacket(17U, 1U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();

            bool captured = await session.CaptureLootRejectedAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0, session.DropListSnapshotV2DropCount);
            Assert.IsTrue(session.LootResolvedCaptured);
            Assert.AreEqual(11UL, session.LootResolved.WinnerSessionId);
            Assert.IsTrue(session.LootRejectedCaptured);
            Assert.AreEqual(17U, session.LootRejected.RoomId);
            Assert.AreEqual(1U, session.LootRejected.DropId);
            Assert.AreEqual(PlayerLootRejectReason.AlreadyClaimed, session.LootRejected.Reason);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
            Assert.AreEqual(10, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureLootRejectedAsyncReportsSpaceLootErrorWithoutDisconnecting()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(ErrorPacket(
                PlayerTcpPacket.ClickLootRequestPacketType,
                PlayerTcpPacket.ErrorCodeNotFound));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();

            bool captured = await session.CaptureLootRejectedAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsFalse(session.LootRejectedCaptured);
            Assert.AreEqual("space loot rejected: NotFound", session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
            Assert.AreEqual(8, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureLootRejectedAsyncKeepsSessionConnectedWhenReceiveTimesOut()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();
            connector.ReceiveHandler = cancellationToken =>
            {
                if (!cancellationToken.CanBeCanceled)
                {
                    throw new InvalidOperationException("receive was not cancellable");
                }

                throw new OperationCanceledException(cancellationToken);
            };

            bool captured = await session.CaptureLootRejectedAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsFalse(session.LootRejectedCaptured);
            Assert.AreEqual("loot rejected receive timeout", session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
            Assert.AreEqual(8, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureLootRejectedAsyncRejectsLocalWinnerWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 22UL, 1001U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();
            await session.CaptureLootResolvedAsync();

            bool captured = await session.CaptureLootRejectedAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsFalse(session.LootRejectedCaptured);
            Assert.AreEqual("loot winner is local session", session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
            Assert.AreEqual(8, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureInventorySnapshotAsyncCapturesWinnerInventoryAfterLootResolved()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 22UL, 1001U, 1));
            connector.ReceivePacketsToReturn.Enqueue(InventorySnapshotPacket(
                22UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 1)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();
            await session.CaptureLootResolvedAsync();

            bool captured = await session.CaptureInventorySnapshotAsync();

            Assert.IsTrue(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsTrue(session.InventorySnapshotCaptured);
            Assert.AreEqual(22UL, session.InventorySnapshot.SessionId);
            Assert.AreEqual(1, session.InventorySnapshot.CurrentWeight);
            Assert.AreEqual(10, session.InventorySnapshot.MaxWeight);
            Assert.AreEqual(1, session.InventorySnapshot.Count);
            Assert.AreEqual(1001U, session.InventorySnapshot.EntryAt(0).ItemId);
            Assert.AreEqual(1, session.InventorySnapshot.EntryAt(0).Quantity);
            Assert.AreEqual(string.Empty, session.LastError);
            Assert.AreEqual(9, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureInventorySnapshotAsyncSkipsFinalResultPacketsUntilInventoryArrives()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL,
                ReportQueuedPacketsAsPending = false
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 22UL, 1001U, 1));
            connector.ReceivePacketsToReturn.Enqueue(BattleFinalRankingPacket(
                17U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 22UL, "Player22", 100L),
                new PlayerBattleFinalRankingRow(2, 11UL, "Player11", 0L)));
            connector.ReceivePacketsToReturn.Enqueue(
                LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.None));
            connector.ReceivePacketsToReturn.Enqueue(InventorySnapshotPacket(
                22UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 1)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            Assert.IsTrue(await session.RequestJoinRoomAsync(17U), session.LastError);
            Assert.IsTrue(await session.RequestReadyRoomAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureBattleStartAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureBattleLoadEntryAsync(), session.LastError);
            Assert.IsTrue(await session.SendArenaLoadCompleteAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureArenaGameplayStartAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureMonsterSpawnAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureMonsterDeathAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureDropListSnapshotV2Async(), session.LastError);
            Assert.IsTrue(await session.SendSpaceLootIntentAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureLootResolvedAsync(), session.LastError);

            bool captured = await session.CaptureInventorySnapshotAsync();

            Assert.IsTrue(captured, session.LastError);
            Assert.IsTrue(session.InventorySnapshotCaptured);
            Assert.IsTrue(session.BattleFinalRankingCaptured);
            Assert.IsTrue(session.LobbyReturnCaptured);
            Assert.AreEqual(22UL, session.InventorySnapshot.SessionId);
            Assert.AreEqual(100L, session.BattleFinalRanking.RowAt(0).TotalAssetValue);
            Assert.AreEqual(PlayerLobbyReturnReason.None, session.LobbyReturnReason);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task CaptureInventorySnapshotAsyncRejectsBeforeLootResolvedWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();

            bool captured = await session.CaptureInventorySnapshotAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsFalse(session.InventorySnapshotCaptured);
            Assert.AreEqual("loot resolved not captured", session.LastError);
            Assert.AreEqual(7, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureInventorySnapshotAsyncRejectsLocalLoserWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 11UL, 1001U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();
            await session.CaptureLootResolvedAsync();

            bool captured = await session.CaptureInventorySnapshotAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsFalse(session.InventorySnapshotCaptured);
            Assert.AreEqual("loot winner is not local session", session.LastError);
            Assert.AreEqual(0, connector.DisconnectCallCount);
            Assert.AreEqual(0, rudpSender.DisconnectCallCount);
            Assert.AreEqual(8, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureInventorySnapshotAsyncRejectsMismatchedSessionAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 1U, 22UL, 1001U, 1));
            connector.ReceivePacketsToReturn.Enqueue(InventorySnapshotPacket(
                33UL,
                1,
                10,
                new PlayerInventoryEntry(1001U, 1)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();
            await session.CaptureLootResolvedAsync();

            bool captured = await session.CaptureInventorySnapshotAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid inventory snapshot packet", session.LastError);
            Assert.IsFalse(session.InventorySnapshotCaptured);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task CaptureLootResolvedAsyncRejectsBeforeSpaceLootWithoutReceiving()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();

            bool captured = await session.CaptureLootResolvedAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.IsFalse(session.LootResolvedCaptured);
            Assert.AreEqual("space loot not sent", session.LastError);
            Assert.AreEqual(7, connector.ReceivePacketCallCount);
        }

        [Test]
        public async Task CaptureLootResolvedAsyncRejectsMismatchedDropAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            connector.ReceivePacketsToReturn.Enqueue(LootResolvedPacket(17U, 99U, 22UL, 1001U, 1));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();
            await session.SendSpaceLootIntentAsync();

            bool captured = await session.CaptureLootResolvedAsync();

            Assert.IsFalse(captured);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("invalid loot resolved packet", session.LastError);
            Assert.IsFalse(session.LootResolvedCaptured);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task SendSpaceLootIntentAsyncRejectsBeforeDropListSnapshotV2WithoutSending()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();

            bool sent = await session.SendSpaceLootIntentAsync();

            Assert.IsFalse(sent);
            Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
            Assert.AreEqual(0, rudpSender.SendSpaceLootCallCount);
            Assert.AreEqual("drop list not captured", session.LastError);
        }

        [Test]
        public async Task SendSpaceLootIntentAsyncFailureMarksFailedAndDisconnects()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
            connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
            connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                17U,
                12345U,
                new PlayerDropEntryV2(1U, 1001U, 1, 0, 0)));
            FakeRudpSender rudpSender = new FakeRudpSender
            {
                SpaceLootExceptionToThrow = new InvalidOperationException("udp space loot failed")
            };
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureMonsterSpawnAsync();
            await session.CaptureMonsterDeathAsync();
            await session.CaptureDropListSnapshotV2Async();

            bool sent = await session.SendSpaceLootIntentAsync();

            Assert.IsFalse(sent);
            Assert.AreEqual(PlayerNetworkSessionStatus.Failed, session.Status);
            Assert.AreEqual("udp space loot failed", session.LastError);
            Assert.AreEqual(0UL, session.SessionId);
            Assert.IsFalse(session.RudpHelloSent);
            Assert.IsFalse(session.RudpSpaceLootSent);
            Assert.AreEqual(1, connector.DisconnectCallCount);
            Assert.AreEqual(1, rudpSender.DisconnectCallCount);
        }

        [Test]
        public async Task CaptureBattleFinalRankingAsyncCapturesMatchingFinalRanking()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleFinalRankingPacket(
                17U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 22UL, "Player22", 100L),
                new PlayerBattleFinalRankingRow(2, 11UL, "Player11", 0L)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();
            await session.CaptureArenaGameplayStartAsync();

            bool captured = await session.CaptureBattleFinalRankingAsync();

            Assert.IsTrue(captured);
            Assert.IsTrue(session.BattleFinalRankingCaptured);
            Assert.AreEqual(17U, session.BattleFinalRanking.RoomId);
            Assert.AreEqual(9001UL, session.BattleFinalRanking.BattleInstanceId);
            Assert.AreEqual(2, session.BattleFinalRanking.Count);
            Assert.AreEqual(22UL, session.BattleFinalRanking.RowAt(0).SessionId);
            Assert.AreEqual(100L, session.BattleFinalRanking.RowAt(0).TotalAssetValue);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task CaptureLobbyReturnVisibilityAsyncPreservesCapturedFinalRanking()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleFinalRankingPacket(
                17U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 22UL, "Player22", 100L),
                new PlayerBattleFinalRankingRow(2, 11UL, "Player11", 0L)));
            connector.ReceivePacketsToReturn.Enqueue(
                LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.None));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();
            await session.CaptureArenaGameplayStartAsync();
            await session.CaptureBattleFinalRankingAsync();

            bool captured = await session.CaptureLobbyReturnVisibilityAsync();

            Assert.IsTrue(captured);
            Assert.IsTrue(session.LobbyReturnCaptured);
            Assert.AreEqual(17U, session.LobbyReturnPreviousRoomId);
            Assert.AreEqual(PlayerLobbyReturnReason.None, session.LobbyReturnReason);
            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.IsTrue(session.BattleFinalRankingCaptured);
            Assert.AreEqual(2, session.BattleFinalRanking.Count);
            Assert.AreEqual(22UL, session.BattleFinalRanking.RowAt(0).SessionId);
            Assert.AreEqual(string.Empty, session.LastError);
        }

        [Test]
        public async Task CaptureBattleFinalRankingAsyncReturnsCachedRankingAfterFrameDrainRace()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();
            await session.CaptureArenaGameplayStartAsync();

            TaskCompletionSource<byte[]> frameDrainReceive = new TaskCompletionSource<byte[]>();
            TaskCompletionSource<byte[]> captureReceive = new TaskCompletionSource<byte[]>();
            int receiveCallCount = 0;
            connector.ReceivePacketsToReturn.Enqueue(new byte[] { 0xFF });
            connector.ReceiveHandler = _ =>
            {
                ++receiveCallCount;
                return receiveCallCount == 1 ?
                    frameDrainReceive.Task :
                    captureReceive.Task;
            };

            Task<bool> frameDrainTask = session.DrainIncomingTcpStateAsync();
            Task<bool> captureTask = session.CaptureBattleFinalRankingAsync();
            frameDrainReceive.SetResult(BattleFinalRankingPacket(
                17U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 11UL, "Player11", 100L),
                new PlayerBattleFinalRankingRow(2, 22UL, "Player22", 0L)));

            try
            {
                Assert.IsTrue(await frameDrainTask, session.LastError);
                Task completedTask = await Task.WhenAny(captureTask, Task.Delay(100));

                Assert.AreSame(
                    captureTask,
                    completedTask,
                    "final ranking capture should return from cached state after frame drain");
                Assert.IsTrue(await captureTask, session.LastError);
                Assert.IsTrue(session.BattleFinalRankingCaptured);
            }
            finally
            {
                if (!captureTask.IsCompleted)
                {
                    captureReceive.SetResult(
                        LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.None));
                    await captureTask;
                }
            }
        }

        [Test]
        public async Task CaptureLobbyReturnVisibilityAsyncReturnsCachedReturnAfterFrameDrainRace()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleFinalRankingPacket(
                17U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 11UL, "Player11", 100L),
                new PlayerBattleFinalRankingRow(2, 22UL, "Player22", 0L)));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();
            await session.CaptureArenaGameplayStartAsync();
            await session.CaptureBattleFinalRankingAsync();

            TaskCompletionSource<byte[]> frameDrainReceive = new TaskCompletionSource<byte[]>();
            TaskCompletionSource<byte[]> captureReceive = new TaskCompletionSource<byte[]>();
            int receiveCallCount = 0;
            connector.ReceivePacketsToReturn.Enqueue(new byte[] { 0xFF });
            connector.ReceiveHandler = _ =>
            {
                ++receiveCallCount;
                return receiveCallCount == 1 ?
                    frameDrainReceive.Task :
                    captureReceive.Task;
            };

            Task<bool> frameDrainTask = session.DrainIncomingTcpStateAsync();
            Task<bool> captureTask = session.CaptureLobbyReturnVisibilityAsync();
            frameDrainReceive.SetResult(
                LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.None));

            try
            {
                Assert.IsTrue(await frameDrainTask, session.LastError);
                Task completedTask = await Task.WhenAny(captureTask, Task.Delay(100));

                Assert.AreSame(
                    captureTask,
                    completedTask,
                    "lobby return capture should return from cached state after frame drain");
                Assert.IsTrue(await captureTask, session.LastError);
                Assert.IsTrue(session.LobbyReturnCaptured);
            }
            finally
            {
                if (!captureTask.IsCompleted)
                {
                    captureReceive.SetResult(
                        RoomListSnapshotPacket(new PlayerRoomListEntry(17U, 0, 10)));
                    await captureTask;
                }
            }
        }

        [Test]
        public async Task RequestCreateRoomAsyncClearsPreviousLobbyReturnCapture()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleFinalRankingPacket(
                17U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 22UL, "Player22", 100L),
                new PlayerBattleFinalRankingRow(2, 11UL, "Player11", 0L)));
            connector.ReceivePacketsToReturn.Enqueue(
                LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.None));
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(23U, 1));
            connector.ReceivePacketsToReturn.Enqueue(
                DefaultCreatedRoomDetailPacket(roomId: 23U, hostSessionId: 22UL));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();
            await session.CaptureArenaGameplayStartAsync();
            await session.CaptureBattleFinalRankingAsync();
            await session.CaptureLobbyReturnVisibilityAsync();

            bool created = await session.RequestCreateRoomAsync("next", 2);

            Assert.IsTrue(created, session.LastError);
            Assert.IsFalse(session.LobbyReturnCaptured);
            Assert.IsFalse(session.BattleFinalRankingCaptured);
            Assert.AreEqual(23U, session.CurrentRoomId);
        }

        [Test]
        public async Task RequestJoinRoomAsyncClearsPreviousLobbyReturnCapture()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleFinalRankingPacket(
                17U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 22UL, "Player22", 100L),
                new PlayerBattleFinalRankingRow(2, 11UL, "Player11", 0L)));
            connector.ReceivePacketsToReturn.Enqueue(
                LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.None));
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(23U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket(roomId: 23U));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestJoinRoomAsync(17U);
            await session.RequestReadyRoomAsync();
            await session.CaptureBattleStartAsync();
            await session.CaptureBattleLoadEntryAsync();
            await session.SendArenaLoadCompleteAsync();
            await session.CaptureArenaGameplayStartAsync();
            await session.CaptureBattleFinalRankingAsync();
            await session.CaptureLobbyReturnVisibilityAsync();

            bool joined = await session.RequestJoinRoomAsync(23U);

            Assert.IsTrue(joined, session.LastError);
            Assert.IsFalse(session.LobbyReturnCaptured);
            Assert.IsFalse(session.BattleFinalRankingCaptured);
            Assert.AreEqual(23U, session.CurrentRoomId);
        }

        [Test]
        public async Task RepeatedMatchCapturesFinalRankingAfterBecomingHost()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 22UL,
                ReportQueuedPacketsAsPending = false
            };
            connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
            connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(17U, 9001UL, 11UL, 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(17U, 9001UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleFinalRankingPacket(
                17U,
                9001UL,
                new PlayerBattleFinalRankingRow(1, 11UL, "Player11", 100L),
                new PlayerBattleFinalRankingRow(2, 22UL, "Player22", 0L)));
            connector.ReceivePacketsToReturn.Enqueue(
                LobbyReturnVisibilityPacket(17U, PlayerLobbyReturnReason.None));
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(23U, 1));
            connector.ReceivePacketsToReturn.Enqueue(
                DefaultCreatedRoomDetailPacket(roomId: 23U, hostSessionId: 22UL));
            connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(23U, 1, 2));
            connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(23U, 22UL, 11UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleLoadEntryPacket(23U, 9002UL, 22UL, 11UL));
            connector.ReceivePacketsToReturn.Enqueue(ArenaGameplayStartPacket(23U, 9002UL));
            connector.ReceivePacketsToReturn.Enqueue(BattleFinalRankingPacket(
                23U,
                9002UL,
                new PlayerBattleFinalRankingRow(1, 11UL, "Player11", 100L),
                new PlayerBattleFinalRankingRow(2, 22UL, "Player22", 0L)));
            connector.ReceivePacketsToReturn.Enqueue(
                LobbyReturnVisibilityPacket(23U, PlayerLobbyReturnReason.None));
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            Assert.IsTrue(await session.RequestJoinRoomAsync(17U), session.LastError);
            Assert.IsTrue(await session.RequestReadyRoomAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureBattleStartAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureBattleLoadEntryAsync(), session.LastError);
            Assert.IsTrue(await session.SendArenaLoadCompleteAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureArenaGameplayStartAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureBattleFinalRankingAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureLobbyReturnVisibilityAsync(), session.LastError);

            Assert.IsTrue(await session.RequestCreateRoomAsync("next", 2), session.LastError);
            Assert.IsTrue(await session.RequestReadyRoomAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureBattleStartAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureBattleLoadEntryAsync(), session.LastError);
            Assert.IsTrue(await session.SendArenaLoadCompleteAsync(), session.LastError);
            Assert.IsTrue(await session.CaptureArenaGameplayStartAsync(), session.LastError);
            bool captured = await session.CaptureBattleFinalRankingAsync();

            Assert.IsTrue(captured, session.LastError);
            Assert.IsTrue(session.BattleFinalRankingCaptured);
            Assert.AreEqual(23U, session.BattleFinalRanking.RoomId);
            Assert.AreEqual(9002UL, session.BattleFinalRanking.BattleInstanceId);
            Assert.AreEqual(2, session.BattleFinalRanking.Count);
            Assert.AreEqual(22UL, session.BattleFinalRanking.RowAt(1).SessionId);
            Assert.IsTrue(await session.CaptureLobbyReturnVisibilityAsync(), session.LastError);
            Assert.IsTrue(session.LobbyReturnCaptured);
            Assert.AreEqual(23U, session.LobbyReturnPreviousRoomId);
        }

        [Test]
        public async Task DisconnectClearsCreateRoomState()
        {
            FakeConnector connector = new FakeConnector
            {
                SessionIdToReturn = 11UL
            };
            connector.ReceivePacketsToReturn.Enqueue(CreateRoomResponsePacket(17U, 1));
            connector.ReceivePacketsToReturn.Enqueue(DefaultCreatedRoomDetailPacket());
            FakeRudpSender rudpSender = new FakeRudpSender();
            PlayerNetworkSession session = new PlayerNetworkSession(connector, rudpSender);
            await session.ConnectAsync(PlayerServerEndpoint.Default);
            await session.RequestCreateRoomAsync();

            session.Disconnect();

            Assert.AreEqual(0U, session.CurrentRoomId);
            Assert.AreEqual(0, session.CurrentRoomPlayerCount);
            Assert.AreEqual(0, session.RoomListCount);
            Assert.AreEqual(new PlayerRoomListEntry[0], session.RoomListEntries);
        }

        private sealed class FakeConnector : IPlayerNetworkConnector
        {
            public int ConnectCallCount;
            public int DisconnectCallCount;
            public int SendPacketCallCount;
            public int ReceivePacketCallCount;
            public PlayerServerEndpoint LastEndpoint;
            public string LastGameSessionToken;
            public byte[] LastSentPacket;
            public Exception ExceptionToThrow;
            public Exception SendExceptionToThrow;
            public ulong SessionIdToReturn = 1UL;
            public ulong[] SnapshotSessionIdsToReturn;
            public PlayerRoomListEntry[] RoomListEntriesToReturn;
            public Func<Task<PlayerNetworkConnectResult>> ConnectHandler;
            public Func<CancellationToken, Task<byte[]>> ReceiveHandler;
            public Queue<byte[]> ReceivePacketsToReturn = new Queue<byte[]>();
            public bool ReportQueuedPacketsAsPending = true;

            public bool HasPendingPacket => ReportQueuedPacketsAsPending && ReceivePacketsToReturn.Count > 0;

            public Task<PlayerNetworkConnectResult> ConnectAsync(
                PlayerServerEndpoint endpoint,
                string gameSessionToken,
                CancellationToken cancellationToken)
            {
                ++ConnectCallCount;
                LastEndpoint = endpoint;
                LastGameSessionToken = gameSessionToken;
                if (ConnectHandler != null)
                {
                    return ConnectHandler();
                }

                if (ExceptionToThrow != null)
                {
                    throw ExceptionToThrow;
                }

                PlayerClientListSnapshot snapshot = new PlayerClientListSnapshot(
                    SnapshotSessionIdsToReturn ?? new[] { SessionIdToReturn });
                PlayerRoomListSnapshot roomListSnapshot = new PlayerRoomListSnapshot(
                    RoomListEntriesToReturn ?? new PlayerRoomListEntry[0]);
                return Task.FromResult(new PlayerNetworkConnectResult(
                    SessionIdToReturn,
                    snapshot,
                    roomListSnapshot));
            }

            public Task SendPacketAsync(byte[] packet, CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                ++SendPacketCallCount;
                LastSentPacket = packet;
                if (SendExceptionToThrow != null)
                {
                    throw SendExceptionToThrow;
                }

                return Task.CompletedTask;
            }

            public Task<byte[]> ReceivePacketAsync(CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                ++ReceivePacketCallCount;
                if (ReceiveHandler != null)
                {
                    return ReceiveHandler(cancellationToken);
                }

                if (ReceivePacketsToReturn.Count == 0)
                {
                    throw new InvalidOperationException("no packet queued");
                }

                return Task.FromResult(ReceivePacketsToReturn.Dequeue());
            }

            public void Disconnect()
            {
                ++DisconnectCallCount;
            }

            public void Dispose()
            {
                Disconnect();
            }
        }

        private sealed class FakeRudpSender : IPlayerRudpSender
        {
            public int SendHelloCallCount;
            public int DisconnectCallCount;
            public PlayerServerEndpoint LastEndpoint;
            public PlayerRudpHello LastHello;
            public Exception ExceptionToThrow;
            public Exception AttackExceptionToThrow;
            public uint SequenceToReturn = 1U;
            public int SendAttackCallCount;
            public ulong LastAttackSessionId;
            public uint LastAttackTargetHintMonsterId;
            public uint AttackSequenceToReturn = 2U;
            public uint AttackCommandSequenceToReturn = 1U;
            public int SendSpaceLootCallCount;
            public ulong LastSpaceLootSessionId;
            public uint SpaceLootSequenceToReturn = 3U;
            public uint SpaceLootCommandSequenceToReturn = 2U;
            public Exception SpaceLootExceptionToThrow;
            public int SendMoveCallCount;
            public ulong LastMoveSessionId;
            public short LastMoveDirX;
            public short LastMoveDirY;
            public uint MoveSequenceToReturn = 4U;
            public uint MoveCommandSequenceToReturn = 3U;
            public PlayerRudpStateSnapshot StateSnapshotToReturn;
            public bool HasStateSnapshotToReturn;

            public Task<PlayerRudpHelloSendResult> SendHelloAsync(
                PlayerServerEndpoint endpoint,
                PlayerRudpHello hello,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                ++SendHelloCallCount;
                LastEndpoint = endpoint;
                LastHello = hello;
                if (ExceptionToThrow != null)
                {
                    throw ExceptionToThrow;
                }

                return Task.FromResult(
                    new PlayerRudpHelloSendResult(hello.SessionId, SequenceToReturn, "127.0.0.1:50000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendAttackAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                uint targetHintMonsterId,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                ++SendAttackCallCount;
                LastEndpoint = endpoint;
                LastAttackSessionId = sessionId;
                LastAttackTargetHintMonsterId = targetHintMonsterId;
                if (AttackExceptionToThrow != null)
                {
                    throw AttackExceptionToThrow;
                }

                return Task.FromResult(new PlayerRudpInputCommandSendResult(
                    sessionId,
                    AttackSequenceToReturn,
                    AttackCommandSequenceToReturn,
                    targetHintMonsterId,
                    "127.0.0.1:50000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendMoveAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                short dirX,
                short dirY,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                ++SendMoveCallCount;
                LastEndpoint = endpoint;
                LastMoveSessionId = sessionId;
                LastMoveDirX = dirX;
                LastMoveDirY = dirY;
                return Task.FromResult(new PlayerRudpInputCommandSendResult(
                    sessionId,
                    MoveSequenceToReturn,
                    MoveCommandSequenceToReturn,
                    0U,
                    "127.0.0.1:50000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendSpaceLootAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                ++SendSpaceLootCallCount;
                LastEndpoint = endpoint;
                LastSpaceLootSessionId = sessionId;
                if (SpaceLootExceptionToThrow != null)
                {
                    throw SpaceLootExceptionToThrow;
                }

                return Task.FromResult(new PlayerRudpInputCommandSendResult(
                    sessionId,
                    SpaceLootSequenceToReturn,
                    SpaceLootCommandSequenceToReturn,
                    0U,
                    "127.0.0.1:50000"));
            }

            public bool TryReceiveStateSnapshot(out PlayerRudpStateSnapshot snapshot)
            {
                if (HasStateSnapshotToReturn || StateSnapshotToReturn.IsValid)
                {
                    snapshot = StateSnapshotToReturn;
                    StateSnapshotToReturn = default;
                    HasStateSnapshotToReturn = false;
                    return true;
                }

                snapshot = default;
                return false;
            }

            public void Disconnect()
            {
                ++DisconnectCallCount;
            }

            public void Dispose()
            {
                Disconnect();
            }
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

        private static byte[] JoinRoomResponsePacket(uint roomId, ushort playerCount)
        {
            byte[] packet = new byte[10];
            packet[1] = 0x0A;
            packet[2] = 0x01;
            packet[3] = 0x04;
            WriteU32BE(roomId, packet, 4);
            packet[8] = (byte)(playerCount >> 8);
            packet[9] = (byte)playerCount;
            return packet;
        }

        private static byte[] ReadyRoomResponsePacket(
            uint roomId,
            ushort readyPlayerCount,
            ushort totalPlayerCount)
        {
            byte[] packet = new byte[12];
            packet[1] = 0x0C;
            packet[2] = 0x01;
            packet[3] = 0x09;
            WriteU32BE(roomId, packet, 4);
            packet[8] = (byte)(readyPlayerCount >> 8);
            packet[9] = (byte)readyPlayerCount;
            packet[10] = (byte)(totalPlayerCount >> 8);
            packet[11] = (byte)totalPlayerCount;
            return packet;
        }

        private static byte[] BattleStartPacket(
            uint roomId,
            ulong playerASessionId,
            ulong playerBSessionId)
        {
            byte[] packet = new byte[24];
            packet[1] = 0x18;
            packet[2] = 0x01;
            packet[3] = 0x0A;
            WriteU32BE(roomId, packet, 4);
            WriteU64BE(playerASessionId, packet, 8);
            WriteU64BE(playerBSessionId, packet, 16);
            return packet;
        }

        private static byte[] BattleLoadEntryPacket(
            uint roomId,
            ulong battleInstanceId,
            params ulong[] playerSessionIds)
        {
            byte[] packet = new byte[18 + (playerSessionIds.Length * 8)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x26;
            WriteU32BE(roomId, packet, 4);
            WriteU64BE(battleInstanceId, packet, 8);
            packet[16] = (byte)(playerSessionIds.Length >> 8);
            packet[17] = (byte)playerSessionIds.Length;
            int offset = 18;
            foreach (ulong playerSessionId in playerSessionIds)
            {
                WriteU64BE(playerSessionId, packet, offset);
                offset += 8;
            }

            return packet;
        }

        private static byte[] ArenaGameplayStartPacket(uint roomId, ulong battleInstanceId)
        {
            byte[] packet = new byte[16];
            packet[1] = 0x10;
            packet[2] = 0x01;
            packet[3] = 0x24;
            WriteU32BE(roomId, packet, 4);
            WriteU64BE(battleInstanceId, packet, 8);
            return packet;
        }

        private static byte[] MonsterSpawnPacket(
            uint roomId,
            uint monsterId,
            uint monsterTypeId,
            ushort maxHp)
        {
            byte[] packet = new byte[18];
            packet[1] = 0x12;
            packet[2] = 0x01;
            packet[3] = 0x0B;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(monsterId, packet, 8);
            WriteU32BE(monsterTypeId, packet, 12);
            packet[16] = (byte)(maxHp >> 8);
            packet[17] = (byte)maxHp;
            return packet;
        }

        private static byte[] MonsterHealthSnapshotPacket(
            uint roomId,
            uint monsterId,
            ushort currentHp,
            ushort maxHp)
        {
            byte[] packet = new byte[16];
            packet[1] = 0x10;
            packet[2] = 0x01;
            packet[3] = 0x19;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(monsterId, packet, 8);
            packet[12] = (byte)(currentHp >> 8);
            packet[13] = (byte)currentHp;
            packet[14] = (byte)(maxHp >> 8);
            packet[15] = (byte)maxHp;
            return packet;
        }

        private static byte[] MonsterDeathPacket(uint roomId, uint monsterId)
        {
            byte[] packet = new byte[12];
            packet[1] = 0x0C;
            packet[2] = 0x01;
            packet[3] = 0x0D;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(monsterId, packet, 8);
            return packet;
        }

        private static byte[] DropListSnapshotV2Packet(
            uint roomId,
            uint scatterSeed,
            params PlayerDropEntryV2[] drops)
        {
            byte[] packet = new byte[14 + (drops.Length * 18)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x1A;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(scatterSeed, packet, 8);
            packet[12] = (byte)(drops.Length >> 8);
            packet[13] = (byte)drops.Length;

            int offset = 14;
            foreach (PlayerDropEntryV2 drop in drops)
            {
                WriteU32BE(drop.DropId, packet, offset);
                WriteU32BE(drop.ItemId, packet, offset + 4);
                packet[offset + 8] = (byte)(drop.Quantity >> 8);
                packet[offset + 9] = (byte)drop.Quantity;
                WriteI32BE(drop.PosX, packet, offset + 10);
                WriteI32BE(drop.PosY, packet, offset + 14);
                offset += 18;
            }

            return packet;
        }

        private static byte[] LootResolvedPacket(
            uint roomId,
            uint dropId,
            ulong winnerSessionId,
            uint itemId,
            ushort quantity)
        {
            byte[] packet = new byte[26];
            packet[1] = 0x1A;
            packet[2] = 0x01;
            packet[3] = 0x10;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(dropId, packet, 8);
            WriteU64BE(winnerSessionId, packet, 12);
            WriteU32BE(itemId, packet, 20);
            packet[24] = (byte)(quantity >> 8);
            packet[25] = (byte)quantity;
            return packet;
        }

        private static byte[] LootRejectedPacket(uint roomId, uint dropId, ushort reason)
        {
            byte[] packet = new byte[14];
            packet[1] = 0x0E;
            packet[2] = 0x01;
            packet[3] = 0x11;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(dropId, packet, 8);
            packet[12] = (byte)(reason >> 8);
            packet[13] = (byte)reason;
            return packet;
        }

        private static byte[] InventorySnapshotPacket(
            ulong sessionId,
            ushort currentWeight,
            ushort maxWeight,
            params PlayerInventoryEntry[] entries)
        {
            byte[] packet = new byte[18 + (entries.Length * 6)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x12;
            WriteU64BE(sessionId, packet, 4);
            packet[12] = (byte)(currentWeight >> 8);
            packet[13] = (byte)currentWeight;
            packet[14] = (byte)(maxWeight >> 8);
            packet[15] = (byte)maxWeight;
            packet[16] = (byte)(entries.Length >> 8);
            packet[17] = (byte)entries.Length;

            int offset = 18;
            foreach (PlayerInventoryEntry entry in entries)
            {
                WriteU32BE(entry.ItemId, packet, offset);
                packet[offset + 4] = (byte)(entry.Quantity >> 8);
                packet[offset + 5] = (byte)entry.Quantity;
                offset += 6;
            }

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

        private static byte[] RoomDetailStatePacket(
            uint roomId,
            PlayerRoomStatus status,
            string title,
            byte maxPlayers,
            ushort selfActionMask,
            params PlayerRoomMemberEntry[] members)
        {
            return RoomDetailStatePacket(
                roomId,
                status,
                title,
                maxPlayers,
                selfActionMask,
                members,
                new PlayerRoomTargetActionEntry[0]);
        }

        private static byte[] RoomDetailStatePacket(
            uint roomId,
            PlayerRoomStatus status,
            string title,
            byte maxPlayers,
            ushort selfActionMask,
            PlayerRoomMemberEntry[] members,
            PlayerRoomTargetActionEntry[] targetActions)
        {
            int size = 4 + 4 + 1 + 1 + title.Length + 1 + 1 + 2 + 1;
            foreach (PlayerRoomMemberEntry member in members)
            {
                size += 4 + 1 + member.Nickname.Length + 1;
            }
            size += targetActions.Length * 6;

            byte[] packet = new byte[size];
            packet[0] = (byte)(size >> 8);
            packet[1] = (byte)size;
            packet[2] = 0x01;
            packet[3] = 0x1D;
            int offset = 4;
            WriteU32BE(roomId, packet, offset);
            offset += 4;
            packet[offset++] = (byte)status;
            packet[offset++] = (byte)title.Length;
            for (int index = 0; index < title.Length; ++index)
            {
                packet[offset++] = (byte)title[index];
            }
            packet[offset++] = maxPlayers;
            packet[offset++] = (byte)members.Length;
            foreach (PlayerRoomMemberEntry member in members)
            {
                WriteU32BE((uint)member.SessionId, packet, offset);
                offset += 4;
                packet[offset++] = (byte)member.Nickname.Length;
                for (int index = 0; index < member.Nickname.Length; ++index)
                {
                    packet[offset++] = (byte)member.Nickname[index];
                }
                packet[offset++] = member.Ready ? (byte)1 : (byte)0;
            }
            packet[offset++] = (byte)(selfActionMask >> 8);
            packet[offset++] = (byte)selfActionMask;
            packet[offset++] = (byte)targetActions.Length;
            foreach (PlayerRoomTargetActionEntry action in targetActions)
            {
                WriteU32BE((uint)action.TargetSessionId, packet, offset);
                offset += 4;
                packet[offset++] = (byte)(action.TargetActionMask >> 8);
                packet[offset++] = (byte)action.TargetActionMask;
            }

            return packet;
        }

        private static byte[] DefaultCreatedRoomDetailPacket(
            uint roomId = 17U,
            ulong hostSessionId = 11UL)
        {
            return RoomDetailStatePacket(
                roomId,
                PlayerRoomStatus.Open,
                $"Room{roomId}",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new PlayerRoomMemberEntry(hostSessionId, $"Player{hostSessionId}", false));
        }

        private static byte[] DefaultJoinedRoomDetailPacket(uint roomId = 17U)
        {
            return RoomDetailStatePacket(
                roomId,
                PlayerRoomStatus.Open,
                $"Room{roomId}",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", false),
                    new PlayerRoomMemberEntry(22UL, "Player22", false)
                },
                new PlayerRoomTargetActionEntry[0]);
        }

        private static byte[] ClientListSnapshotPacket(params ulong[] sessionIds)
        {
            byte[] packet = new byte[6 + (sessionIds.Length * 8)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x00;
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

        private static byte[] LobbyReturnVisibilityPacket(
            uint previousRoomId,
            PlayerLobbyReturnReason reason)
        {
            byte[] packet = new byte[9];
            packet[0] = 0x00;
            packet[1] = 0x09;
            packet[2] = 0x01;
            packet[3] = 0x1C;
            WriteU32BE(previousRoomId, packet, 4);
            packet[8] = (byte)reason;
            return packet;
        }

        private static byte[] BattleFinalRankingPacket(
            uint roomId,
            ulong battleInstanceId,
            params PlayerBattleFinalRankingRow[] rows)
        {
            int size = 17;
            foreach (PlayerBattleFinalRankingRow row in rows)
            {
                byte[] nicknameBytes = System.Text.Encoding.UTF8.GetBytes(row.Nickname);
                size += 2 + 8 + 1 + nicknameBytes.Length + 8;
            }

            byte[] packet = new byte[size];
            packet[0] = (byte)(size >> 8);
            packet[1] = (byte)size;
            packet[2] = 0x01;
            packet[3] = 0x25;
            WriteU32BE(roomId, packet, 4);
            WriteU64BE(battleInstanceId, packet, 8);
            packet[16] = (byte)rows.Length;

            int offset = 17;
            foreach (PlayerBattleFinalRankingRow row in rows)
            {
                packet[offset++] = (byte)(row.Rank >> 8);
                packet[offset++] = (byte)row.Rank;
                WriteU64BE(row.SessionId, packet, offset);
                offset += 8;
                byte[] nicknameBytes = System.Text.Encoding.UTF8.GetBytes(row.Nickname);
                packet[offset++] = (byte)nicknameBytes.Length;
                Buffer.BlockCopy(nicknameBytes, 0, packet, offset, nicknameBytes.Length);
                offset += nicknameBytes.Length;
                WriteI64BE(row.TotalAssetValue, packet, offset);
                offset += 8;
            }

            return packet;
        }

        private static byte[] ErrorPacket(ushort failedType, ushort errorCode)
        {
            byte[] packet = new byte[8];
            packet[1] = 0x08;
            packet[2] = 0x01;
            packet[3] = 0xFF;
            packet[4] = (byte)(failedType >> 8);
            packet[5] = (byte)failedType;
            packet[6] = (byte)(errorCode >> 8);
            packet[7] = (byte)errorCode;
            return packet;
        }

        private static byte[] SessionReplacedPacket()
        {
            return new byte[]
            {
                0x00, 0x04,
                0x00, 0x04
            };
        }

        private static void SetRoomVisibilityCommandInFlight(
            PlayerNetworkSession session,
            bool value)
        {
            FieldInfo field = typeof(PlayerNetworkSession).GetField(
                "roomVisibilityCommandInFlight",
                BindingFlags.Instance | BindingFlags.NonPublic);
            field.SetValue(session, value);
        }

        private static byte[] RoomIdPacket(ushort packetType, uint roomId)
        {
            byte[] packet = new byte[8];
            packet[0] = 0x00;
            packet[1] = 0x08;
            packet[2] = (byte)(packetType >> 8);
            packet[3] = (byte)packetType;
            WriteU32BE(roomId, packet, 4);
            return packet;
        }

        private static void WriteU32BE(uint value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 24);
            packet[offset + 1] = (byte)(value >> 16);
            packet[offset + 2] = (byte)(value >> 8);
            packet[offset + 3] = (byte)value;
        }

        private static void WriteU64BE(ulong value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 56);
            packet[offset + 1] = (byte)(value >> 48);
            packet[offset + 2] = (byte)(value >> 40);
            packet[offset + 3] = (byte)(value >> 32);
            packet[offset + 4] = (byte)(value >> 24);
            packet[offset + 5] = (byte)(value >> 16);
            packet[offset + 6] = (byte)(value >> 8);
            packet[offset + 7] = (byte)value;
        }

        private static void WriteI32BE(int value, byte[] packet, int offset)
        {
            WriteU32BE(unchecked((uint)value), packet, offset);
        }

        private static void WriteI64BE(long value, byte[] packet, int offset)
        {
            WriteU64BE(unchecked((ulong)value), packet, offset);
        }
    }
}
