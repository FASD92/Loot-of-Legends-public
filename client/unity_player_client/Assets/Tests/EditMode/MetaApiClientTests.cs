using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class MetaApiClientTests
    {
        [Test]
        public void BuildsExpectedEndpointPaths()
        {
            MetaApiClient client = new MetaApiClient();

            Assert.AreEqual("/api/release0/auth/session", client.SessionPath);
            Assert.AreEqual("/api/release0/auth/nickname", client.NicknamePath);
            Assert.AreEqual("/api/release0/auth/nickname/check", client.NicknameCheckPath);
            Assert.AreEqual("/api/release0/admission/enter", client.AdmissionPath);
            Assert.AreEqual("/api/release0/admission/status", client.AdmissionStatusPath);
            Assert.AreEqual("/api/release0/admission/queue", client.QueueCancelPath());
            Assert.AreEqual(
                "/api/release0/admission/queue/queue-token-1",
                client.QueueStatusPath("queue-token-1"));
            Assert.AreEqual(
                "/api/release0/admission/queue/queue-token-1",
                client.QueueCancelPath("queue-token-1"));
        }

        [Test]
        public void BuildsCurrentMetaServerAdmissionRequests()
        {
            MetaApiClient client = new MetaApiClient();

            MetaApiRequest enter = client.BuildAdmissionRequest("portfolio-2026");
            Assert.AreEqual("POST", enter.Method);
            Assert.AreEqual("/api/release0/admission/enter", enter.Path);
            StringAssert.Contains("\"inviteCode\":\"portfolio-2026\"", enter.BodyJson);

            MetaApiRequest accountStatus = client.BuildAdmissionStatusRequest();
            Assert.AreEqual("GET", accountStatus.Method);
            Assert.AreEqual("/api/release0/admission/status", accountStatus.Path);

            MetaApiRequest tokenStatus = client.BuildQueueStatusRequest("queue-token-1");
            Assert.AreEqual("GET", tokenStatus.Method);
            Assert.AreEqual(
                "/api/release0/admission/queue/queue-token-1",
                tokenStatus.Path);

            MetaApiRequest accountCancel = client.BuildQueueCancelRequest();
            Assert.AreEqual("DELETE", accountCancel.Method);
            Assert.AreEqual("/api/release0/admission/queue", accountCancel.Path);

            MetaApiRequest tokenCancel = client.BuildQueueCancelRequest("queue-token-1");
            Assert.AreEqual("DELETE", tokenCancel.Method);
            Assert.AreEqual(
                "/api/release0/admission/queue/queue-token-1",
                tokenCancel.Path);
        }

        [Test]
        public void BuildsAdmissionRequestWithBlankInviteCodeWhenMissing()
        {
            MetaApiClient client = new MetaApiClient();

            MetaApiRequest request = client.BuildAdmissionRequest(null);

            Assert.AreEqual("POST", request.Method);
            Assert.AreEqual("/api/release0/admission/enter", request.Path);
            StringAssert.Contains("\"inviteCode\":\"\"", request.BodyJson);
        }

        [Test]
        public void BuildsStandaloneExchangeRequest()
        {
            MetaApiClient client = new MetaApiClient();

            MetaApiRequest request =
                client.BuildStandaloneExchangeRequest("code-1", "state-1");

            Assert.AreEqual("POST", request.Method);
            Assert.AreEqual(
                "/api/release0/auth/standalone/exchange",
                request.Path);
            StringAssert.Contains("\"code\":\"code-1\"", request.BodyJson);
            StringAssert.Contains("\"state\":\"state-1\"", request.BodyJson);
        }

        [Test]
        public void BuildsNicknameCheckRequest()
        {
            MetaApiClient client = new MetaApiClient();

            MetaApiRequest request = client.BuildNicknameCheckRequest("Ready26");

            Assert.AreEqual("POST", request.Method);
            Assert.AreEqual("/api/release0/auth/nickname/check", request.Path);
            StringAssert.Contains("\"nickname\":\"Ready26\"", request.BodyJson);
        }

        [Test]
        public void ParsesNicknameAvailabilityJson()
        {
            Assert.True(MetaApiClient.TryParseNicknameAvailabilityJson(
                "{\"available\":true,\"message\":\"사용 가능한 플레이어 이름입니다\"}",
                out MetaNicknameAvailability availability,
                out string error));

            Assert.True(availability.Available);
            Assert.AreEqual("사용 가능한 플레이어 이름입니다", availability.Message);
            Assert.AreEqual(string.Empty, error);
        }

        [Test]
        public void ParsesAuthenticatedSessionJson()
        {
            Assert.True(MetaApiClient.TryParseAuthSessionJson(
                "{\"authenticated\":true,\"accountId\":7,\"nicknameRequired\":false,\"nickname\":\"Player7\"}",
                out MetaAuthSession session,
                out string error));

            Assert.True(session.Authenticated);
            Assert.AreEqual(7L, session.AccountId);
            Assert.False(session.NicknameRequired);
            Assert.AreEqual("Player7", session.Nickname);
            Assert.AreEqual(string.Empty, error);
        }

        [Test]
        public void MapsAdmittedAdmissionResponseWithoutNetworkDependency()
        {
            MetaAdmissionResponse response = new MetaAdmissionResponse
            {
                state = "ADMITTED",
                gameSessionToken = "game-token-1",
                gameServerHost = "127.0.0.1",
                tcpPort = 40000,
                rudpPort = 40000,
                reservationExpiresAt = 1_785_000_000_001L
            };

            Assert.True(MetaApiClient.TryMapAdmissionResponse(
                response,
                out MetaQueueStatus status,
                out MetaAdmissionResult admission,
                out string error));

            Assert.AreEqual(MetaQueueState.Admitted, status.State);
            Assert.AreEqual(0, status.Position);
            Assert.AreEqual("game-token-1", admission.GameSessionToken);
            Assert.AreEqual("127.0.0.1", admission.GameServerEndpoint.Host);
            Assert.AreEqual(40000, admission.GameServerEndpoint.TcpPort);
            Assert.AreEqual(40000, admission.GameServerEndpoint.RudpPort);
            Assert.AreEqual(1_785_000_000_001L, admission.ReservationExpiresAt);
            Assert.AreEqual(string.Empty, error);
        }

        [Test]
        public void MapsQueuedAdmissionResponseWithoutNetworkDependency()
        {
            MetaAdmissionResponse response = new MetaAdmissionResponse
            {
                state = "QUEUED",
                position = 23,
                queueToken = "queue-token-23"
            };

            Assert.True(MetaApiClient.TryMapAdmissionResponse(
                response,
                out MetaQueueStatus status,
                out MetaAdmissionResult admission,
                out string error));

            Assert.AreEqual(MetaQueueState.Queued, status.State);
            Assert.AreEqual(23, status.Position);
            Assert.AreEqual("queue-token-23", status.QueueToken);
            Assert.AreEqual(string.Empty, admission.GameSessionToken);
            Assert.AreEqual(string.Empty, error);
        }

        [Test]
        public void MapsCurrentMetaServerStatusFieldAndNestedEndpoint()
        {
            MetaAdmissionResponse response = new MetaAdmissionResponse
            {
                status = "Admitted",
                gameSessionToken = "server-token-1",
                reservationExpiresAt = 1_785_000_000_002L,
                gameServerEndpoint = new MetaGameServerEndpointResponse
                {
                    host = "127.0.0.1",
                    tcpPort = 40000,
                    rudpPort = 40000
                }
            };

            Assert.True(MetaApiClient.TryMapAdmissionResponse(
                response,
                out MetaQueueStatus status,
                out MetaAdmissionResult admission,
                out string error));

            Assert.AreEqual(MetaQueueState.Admitted, status.State);
            Assert.AreEqual("server-token-1", admission.GameSessionToken);
            Assert.AreEqual("127.0.0.1", admission.GameServerEndpoint.Host);
            Assert.AreEqual(40000, admission.GameServerEndpoint.TcpPort);
            Assert.AreEqual(40000, admission.GameServerEndpoint.RudpPort);
            Assert.AreEqual(1_785_000_000_002L, admission.ReservationExpiresAt);
            Assert.AreEqual(string.Empty, error);
        }

        [Test]
        public void RejectsAdmittedAdmissionWithoutPositiveReservationDeadline()
        {
            MetaAdmissionResponse missingDeadline = new MetaAdmissionResponse
            {
                status = "Admitted",
                gameSessionToken = "server-token-1",
                gameServerEndpoint = new MetaGameServerEndpointResponse
                {
                    host = "127.0.0.1",
                    tcpPort = 40000,
                    rudpPort = 40000
                }
            };

            Assert.False(MetaApiClient.TryMapAdmissionResponse(
                missingDeadline,
                out _,
                out MetaAdmissionResult missingAdmission,
                out string missingError));
            Assert.False(missingAdmission.IsValid);
            Assert.AreEqual("invalid admission reservation deadline", missingError);

            missingDeadline.reservationExpiresAt = 0L;
            Assert.False(MetaApiClient.TryMapAdmissionResponse(
                missingDeadline,
                out _,
                out _,
                out string zeroError));
            Assert.AreEqual("invalid admission reservation deadline", zeroError);
        }

        [Test]
        public void MapsQueueExpiredAndNotQueuedStatusResponses()
        {
            Assert.True(MetaApiClient.TryMapQueueStatusResponse(
                new MetaQueueStatusResponse { state = "QUEUE_EXPIRED" },
                out MetaQueueStatus expired,
                out string expiredError));
            Assert.AreEqual(MetaQueueState.QueueExpired, expired.State);
            Assert.AreEqual(string.Empty, expiredError);

            Assert.True(MetaApiClient.TryMapQueueStatusResponse(
                new MetaQueueStatusResponse { state = "NOT_QUEUED" },
                out MetaQueueStatus notQueued,
                out string notQueuedError));
            Assert.AreEqual(MetaQueueState.NotQueued, notQueued.State);
            Assert.AreEqual(string.Empty, notQueuedError);
        }

        [Test]
        public void MapsGameServerReadinessFailedWithoutAdmissionHandoff()
        {
            Assert.True(MetaApiClient.TryMapAdmissionResponse(
                new MetaAdmissionResponse { status = "GameServerReadinessFailed" },
                out MetaQueueStatus status,
                out MetaAdmissionResult admission,
                out string error));

            Assert.AreEqual(MetaQueueState.GameServerReadinessFailed, status.State);
            Assert.False(admission.IsValid);
            Assert.AreEqual(string.Empty, admission.GameSessionToken);
            Assert.AreEqual(string.Empty, error);
        }

        [Test]
        public void ParsesAdmittedQueueStatusWithAdmissionResultForLoginSessionRoot()
        {
            const string json =
                "{\"status\":\"Admitted\"," +
                "\"gameSessionToken\":\"queue-admitted-token\"," +
                "\"reservationExpiresAt\":1785000000003," +
                "\"gameServerEndpoint\":{" +
                "\"host\":\"game.internal\"," +
                "\"tcpPort\":41000," +
                "\"rudpPort\":41001}}";

            Assert.True(MetaApiClient.TryParseQueueStatusJson(
                json,
                out MetaQueueStatus status,
                out MetaAdmissionResult admission,
                out string error));
            Assert.AreEqual(MetaQueueState.Admitted, status.State);
            Assert.AreEqual("queue-admitted-token", admission.GameSessionToken);
            Assert.AreEqual("game.internal", admission.GameServerEndpoint.Host);
            Assert.AreEqual(41000, admission.GameServerEndpoint.TcpPort);
            Assert.AreEqual(41001, admission.GameServerEndpoint.RudpPort);
            Assert.AreEqual(1_785_000_000_003L, admission.ReservationExpiresAt);
            Assert.AreEqual(string.Empty, error);

            GameObject rootObject = new GameObject("LoginSessionRoot");
            try
            {
                GameSessionRoot sessionRoot = rootObject.AddComponent<GameSessionRoot>();
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                controller.SessionRoot = sessionRoot;

                Assert.True(controller.ApplyAdmissionResult(status, admission));
                Assert.True(sessionRoot.HasAdmission);
                Assert.AreEqual("queue-admitted-token", sessionRoot.GameSessionToken);
                Assert.AreEqual("game.internal", sessionRoot.GameServerEndpoint.Host);
                Assert.AreEqual(1_785_000_000_003L, sessionRoot.ReservationExpiresAt);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void RejectsAdmittedQueueStatusWithoutPositiveReservationDeadline()
        {
            const string json =
                "{\"status\":\"Admitted\"," +
                "\"gameSessionToken\":\"queue-admitted-token\"," +
                "\"reservationExpiresAt\":0," +
                "\"gameServerEndpoint\":{" +
                "\"host\":\"game.internal\"," +
                "\"tcpPort\":41000," +
                "\"rudpPort\":41001}}";

            Assert.False(MetaApiClient.TryParseQueueStatusJson(
                json,
                out MetaQueueStatus status,
                out MetaAdmissionResult admission,
                out string error));
            Assert.AreEqual(default(MetaQueueStatus).State, status.State);
            Assert.False(admission.IsValid);
            Assert.AreEqual("invalid admission reservation deadline", error);
        }
    }
}
