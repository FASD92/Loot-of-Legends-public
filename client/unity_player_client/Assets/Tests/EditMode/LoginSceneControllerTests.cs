using LootOfLegends.PlayerClient;
using NUnit.Framework;
using System.IO;
using System.Threading.Tasks;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class LoginSceneControllerTests
    {
        [Test]
        public void ShowsQueuePositionWithoutEtaWhenQueued()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller = rootObject.AddComponent<LoginSceneController>();

                controller.ApplyQueueStatus(new MetaQueueStatus(MetaQueueState.Queued, 17));

                Assert.AreEqual("대기열 17번째", controller.StatusText);
                StringAssert.DoesNotContain("ETA", controller.StatusText);
                StringAssert.DoesNotContain("예상", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void ShowsGameServerReadinessFailureCopy()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller = rootObject.AddComponent<LoginSceneController>();

                controller.ApplyQueueStatus(
                    new MetaQueueStatus(MetaQueueState.GameServerReadinessFailed, 0));

                Assert.AreEqual(
                    LoginSceneController.GameServerReadinessFailureMessage,
                    controller.StatusText);
                Assert.AreEqual(0.0f, controller.NextQueuePollDelaySeconds);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void ShowsCauseSpecificNicknameValidationCopy()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller = rootObject.AddComponent<LoginSceneController>();

                controller.ApplyNicknameValidationFailure(
                    MetaNicknameValidationFailure.UnsupportedCharacters);
                Assert.AreEqual(
                    "닉네임은 한글, 영문, 숫자만 사용할 수 있습니다",
                    controller.StatusText);

                controller.ApplyNicknameValidationFailure(
                    MetaNicknameValidationFailure.Duplicate);
                Assert.AreEqual("이미 사용 중인 닉네임입니다", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void ExposesFiveSecondQueuePollingIntervalWithoutCppRoomPolling()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller = rootObject.AddComponent<LoginSceneController>();

                Assert.AreEqual(5.0f, LoginSceneController.QueuePollingIntervalSeconds);
                controller.ApplyQueueStatus(new MetaQueueStatus(MetaQueueState.Queued, 4));

                Assert.AreEqual(5.0f, controller.NextQueuePollDelaySeconds);
                Assert.False(controller.PollsGameRoomListBeforeAdmission);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void ExposesRelease0KoreanLoginLabels()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller = rootObject.AddComponent<LoginSceneController>();

                Assert.AreEqual("Loot of Legends", controller.GameTitleText);
                Assert.AreEqual("Google로 로그인", controller.GoogleLoginButtonText);
                Assert.AreEqual("플레이어 이름", controller.PlayerNameLabelText);
                Assert.AreEqual(
                    "처음 시작하기",
                    controller.StartPlayerNameSetupButtonText);
                Assert.AreEqual("중복 확인", controller.CheckPlayerNameButtonText);
                Assert.AreEqual("Google 인증 완료", controller.GoogleAuthCompleteButtonText);
                Assert.AreEqual(
                    "생성",
                    controller.CreatePlayerNameButtonText);
                Assert.AreEqual(
                    "플레이어 이름 설정이 필요합니다",
                    LoginSceneController.NicknameRequiredMessage);
                Assert.AreEqual("입장하기", controller.AdmissionButtonText);
                Assert.AreEqual("초대 코드", controller.InviteCodeLabelText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void LoginPanelAreaLeavesRoomForInviteCodeControls()
        {
            Rect panelRect = LoginSceneController.CalculateWelcomePanelRect(1280, 720);

            Assert.AreEqual(380.0f, panelRect.x);
            Assert.AreEqual(140.0f, panelRect.y);
            Assert.AreEqual(520.0f, panelRect.width);
            Assert.AreEqual(440.0f, panelRect.height);
        }

        [Test]
        public void PlayerNameModalCanOpenCloseAndUsesCenteredRect()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller = rootObject.AddComponent<LoginSceneController>();

                Assert.IsFalse(controller.PlayerNameModalVisible);

                controller.OpenPlayerNameModal();
                Assert.IsTrue(controller.PlayerNameModalVisible);

                Rect modalRect = LoginSceneController.CalculatePlayerNameModalRect(1280, 720);
                Assert.AreEqual(430.0f, modalRect.x);
                Assert.AreEqual(190.0f, modalRect.y);
                Assert.AreEqual(420.0f, modalRect.width);
                Assert.AreEqual(340.0f, modalRect.height);
                Assert.Less(
                    modalRect.width,
                    LoginSceneController.CalculateWelcomePanelRect(1280, 720).width);

                controller.ClosePlayerNameModal();
                Assert.IsFalse(controller.PlayerNameModalVisible);
                Assert.AreEqual(string.Empty, controller.PlayerNameInput);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void PlayerNameModalUsesOpaquePanelAndLocksWelcomeInput()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller = rootObject.AddComponent<LoginSceneController>();

                Assert.IsTrue(controller.WelcomePanelInputEnabled);
                controller.OpenPlayerNameModal();

                Assert.IsFalse(controller.WelcomePanelInputEnabled);
                Assert.GreaterOrEqual(
                    LoginSceneController.PlayerNameModalBackdropColor.a,
                    0.70f);
                Assert.GreaterOrEqual(
                    LoginSceneController.PlayerNameModalPanelColor.a,
                    1.0f);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void PlayerNameModalLeavesRoomForStatusAndAllButtons()
        {
            float minimumContentHeight =
                (PlayerManualGuiScaler.LabelFixedHeight * 2.0f) +
                PlayerManualGuiScaler.TextFieldFixedHeight +
                (PlayerManualGuiScaler.ButtonFixedHeight * 4.0f) +
                24.0f;

            Assert.GreaterOrEqual(
                LoginSceneController.PlayerNameModalHeight,
                minimumContentHeight);
        }

        [Test]
        public async Task PlayerNameCheckEnablesGoogleAuthOnlyForAvailableName()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                FakeMetaSessionClient metaClient = new FakeMetaSessionClient
                {
                    NicknameCheckResult =
                        MetaHttpResult<MetaNicknameAvailability>.Success(
                            new MetaNicknameAvailability(
                                true,
                                "사용 가능한 플레이어 이름입니다"),
                            200)
                };
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.MetaSessionClient = metaClient;
                controller.OpenPlayerNameModal();
                controller.PlayerNameInput = "Ready26";

                Assert.True(controller.CanCheckPlayerName);
                Assert.False(controller.CanAuthenticatePlayerName);
                Assert.False(controller.CanCreatePlayerName);

                Assert.True(await controller.RunPlayerNameCheckFlowAsync());

                Assert.AreEqual("https://meta.example.com", metaClient.NicknameCheckMetaBaseUrl);
                Assert.AreEqual("Ready26", metaClient.NicknameCheck);
                Assert.True(controller.CanAuthenticatePlayerName);
                Assert.False(controller.CanCreatePlayerName);
                Assert.AreEqual("사용 가능한 플레이어 이름입니다", controller.PlayerNameModalStatusText);
                Assert.AreNotEqual("사용 가능한 플레이어 이름입니다", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public async Task PlayerNameChangeClearsAvailabilityAndDisablesGoogleAuth()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                controller.MetaSessionClient = new FakeMetaSessionClient
                {
                    NicknameCheckResult =
                        MetaHttpResult<MetaNicknameAvailability>.Success(
                            new MetaNicknameAvailability(
                                true,
                                "사용 가능한 플레이어 이름입니다"),
                            200)
                };
                controller.PlayerNameInput = "Ready26";

                Assert.True(await controller.RunPlayerNameCheckFlowAsync());
                Assert.True(controller.CanAuthenticatePlayerName);

                controller.PlayerNameInput = "Ready27";

                Assert.True(controller.CanCheckPlayerName);
                Assert.False(controller.CanAuthenticatePlayerName);
                Assert.False(controller.CanCreatePlayerName);
                Assert.AreEqual(string.Empty, controller.PlayerNameModalStatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public async Task PlayerNameCheckUnavailableKeepsGoogleAuthDisabled()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                controller.MetaSessionClient = new FakeMetaSessionClient
                {
                    NicknameCheckResult =
                        MetaHttpResult<MetaNicknameAvailability>.Success(
                            new MetaNicknameAvailability(
                                false,
                                "이미 사용 중인 플레이어 이름입니다"),
                            200)
                };
                controller.PlayerNameInput = "Ready26";

                Assert.False(await controller.RunPlayerNameCheckFlowAsync());

                Assert.False(controller.CanAuthenticatePlayerName);
                Assert.False(controller.CanCreatePlayerName);
                Assert.AreEqual("이미 사용 중인 플레이어 이름입니다", controller.PlayerNameModalStatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void TryStartLocalDevAdmissionStoresDevTokenAndEndpoint()
        {
            GameObject sessionRootObject = new GameObject("GameSessionRoot");
            GameObject controllerObject = new GameObject("LoginSceneController");
            try
            {
                GameSessionRoot sessionRoot = sessionRootObject.AddComponent<GameSessionRoot>();
                LoginSceneController controller =
                    controllerObject.AddComponent<LoginSceneController>();
                controller.SessionRoot = sessionRoot;

                Assert.IsTrue(controller.TryStartLocalDevAdmission("player1"));

                Assert.IsTrue(sessionRoot.HasAdmission);
                Assert.AreEqual("dev-session:player1", sessionRoot.GameSessionToken);
                Assert.AreEqual(PlayerServerEndpoint.Default.DisplayName,
                    sessionRoot.GameServerEndpoint.DisplayName);
                Assert.AreEqual(LoginSceneController.LocalDevAdmissionMessage,
                    controller.StatusText);
                Assert.IsFalse(controller.ShouldLoadLobbyAfterAdmission);
            }
            finally
            {
                Object.DestroyImmediate(controllerObject);
                Object.DestroyImmediate(sessionRootObject);
            }
        }

        [Test]
        public void TryStartLocalDevAdmissionRejectsInvalidNickname()
        {
            GameObject sessionRootObject = new GameObject("GameSessionRoot");
            GameObject controllerObject = new GameObject("LoginSceneController");
            try
            {
                GameSessionRoot sessionRoot = sessionRootObject.AddComponent<GameSessionRoot>();
                LoginSceneController controller =
                    controllerObject.AddComponent<LoginSceneController>();
                controller.SessionRoot = sessionRoot;

                Assert.IsFalse(controller.TryStartLocalDevAdmission("p!"));

                Assert.IsFalse(sessionRoot.HasAdmission);
                Assert.AreEqual(
                    "닉네임은 한글, 영문, 숫자만 사용할 수 있습니다",
                    controller.StatusText);
                Assert.IsFalse(controller.ShouldLoadLobbyAfterAdmission);
            }
            finally
            {
                Object.DestroyImmediate(controllerObject);
                Object.DestroyImmediate(sessionRootObject);
            }
        }

        [Test]
        public void GoogleLoginFailureShowsStandaloneHandoffFailureCopy()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();

                controller.ApplyStandaloneOAuthHandoffFailure();

                Assert.AreEqual(
                    "Google 로그인 연결에 실패했습니다. 다시 시도해주세요",
                    controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void AuthenticatedSessionWithNicknameCanStartAdmission()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();

                controller.ApplyAuthenticatedMetaSession(
                    new MetaAuthSession(true, 7L, false, "Player7"));

                Assert.AreEqual("입장하기", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public async Task StandaloneOAuthLoginFlowAppliesAuthenticatedSession()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                FakeStandaloneOAuthLoginFlow oauthFlow =
                    new FakeStandaloneOAuthLoginFlow(
                        MetaHttpResult<MetaAuthSession>.Success(
                            new MetaAuthSession(true, 7L, false, "Player7"),
                            200));
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.StandaloneOAuthLoginFlow = oauthFlow;

                Assert.True(await controller.RunStandaloneOAuthLoginFlowAsync());

                Assert.AreEqual("https://meta.example.com", oauthFlow.MetaBaseUrl);
                Assert.AreEqual("입장하기", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public async Task PlayerNameGoogleAuthForExistingAccountClosesModal()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                controller.MetaSessionClient = new FakeMetaSessionClient
                {
                    NicknameCheckResult =
                        MetaHttpResult<MetaNicknameAvailability>.Success(
                            new MetaNicknameAvailability(
                                true,
                                "사용 가능한 플레이어 이름입니다"),
                            200)
                };
                controller.StandaloneOAuthLoginFlow =
                    new FakeStandaloneOAuthLoginFlow(
                        MetaHttpResult<MetaAuthSession>.Success(
                            new MetaAuthSession(true, 7L, false, "Player7"),
                            200));
                controller.OpenPlayerNameModal();
                controller.PlayerNameInput = "UnusedName";
                Assert.True(await controller.RunPlayerNameCheckFlowAsync());

                Assert.True(await controller.RunPlayerNameGoogleAuthFlowAsync());

                Assert.IsFalse(controller.PlayerNameModalVisible);
                Assert.AreEqual("입장하기", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public async Task PlayerNameCreateRequiresCheckedNameAndCompletedGoogleAuth()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                FakeMetaSessionClient metaClient = new FakeMetaSessionClient
                {
                    NicknameCheckResult =
                        MetaHttpResult<MetaNicknameAvailability>.Success(
                            new MetaNicknameAvailability(
                                true,
                                "사용 가능한 플레이어 이름입니다"),
                            200),
                    NicknameResult = MetaHttpResult<MetaAuthSession>.Success(
                        new MetaAuthSession(true, 7L, false, "Ready26"),
                        200)
                };
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.MetaSessionClient = metaClient;
                controller.StandaloneOAuthLoginFlow =
                    new FakeStandaloneOAuthLoginFlow(
                        MetaHttpResult<MetaAuthSession>.Success(
                            new MetaAuthSession(true, 7L, true, string.Empty),
                            200));
                controller.OpenPlayerNameModal();
                controller.PlayerNameInput = "Ready26";

                Assert.True(await controller.RunPlayerNameCheckFlowAsync());
                Assert.False(controller.CanCreatePlayerName);
                Assert.True(await controller.RunPlayerNameGoogleAuthFlowAsync());
                Assert.True(controller.CanCreatePlayerName);
                Assert.True(await controller.RunPlayerNameCreateFlowAsync("Ready26"));

                Assert.AreEqual("Ready26", metaClient.Nickname);
                Assert.IsFalse(controller.PlayerNameModalVisible);
                Assert.AreEqual("입장하기", controller.StatusText);
                Assert.AreEqual(string.Empty, controller.PlayerNameInput);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public async Task StandaloneOAuthLoginFlowUsesStreamingAssetsMetaBaseUrl()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            string tempDirectory = Path.Combine(
                Path.GetTempPath(),
                "release0-login-config-" + System.Guid.NewGuid().ToString("N"));
            try
            {
                Directory.CreateDirectory(tempDirectory);
                File.WriteAllText(
                    Path.Combine(tempDirectory, Release0ClientConfig.FileName),
                    "{ \"metaBaseUrl\": \"https://meta.release0.example.com/\" }");

                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                FakeStandaloneOAuthLoginFlow oauthFlow =
                    new FakeStandaloneOAuthLoginFlow(
                        MetaHttpResult<MetaAuthSession>.Success(
                            new MetaAuthSession(true, 7L, false, "Player7"),
                            200));
                controller.MetaBaseUrl = Release0ClientConfig.DefaultMetaBaseUrl;
                controller.LoadRelease0ClientConfigFrom(tempDirectory);
                controller.StandaloneOAuthLoginFlow = oauthFlow;

                Assert.True(await controller.RunStandaloneOAuthLoginFlowAsync());

                Assert.AreEqual(
                    "https://meta.release0.example.com",
                    oauthFlow.MetaBaseUrl);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
                if (Directory.Exists(tempDirectory))
                {
                    Directory.Delete(tempDirectory, true);
                }
            }
        }

        [Test]
        public async Task StandaloneOAuthLoginFlowExceptionReleasesRetryGate()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                controller.StandaloneOAuthLoginFlow =
                    new ThrowingStandaloneOAuthLoginFlow();

                Assert.ThrowsAsync<System.InvalidOperationException>(
                    async () => await controller.RunStandaloneOAuthLoginFlowAsync());

                controller.StandaloneOAuthLoginFlow =
                    new FakeStandaloneOAuthLoginFlow(
                        MetaHttpResult<MetaAuthSession>.Success(
                            new MetaAuthSession(true, 7L, false, "Player7"),
                            200));

                Assert.True(await controller.RunStandaloneOAuthLoginFlowAsync());
                Assert.AreEqual("입장하기", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public async Task MetaAdmissionFlowStoresAdmissionAndLoadsLobbyAfterCppConnect()
        {
            GameObject sessionRootObject = new GameObject("GameSessionRoot");
            GameObject controllerObject = new GameObject("LoginSceneController");
            try
            {
                GameSessionRoot sessionRoot =
                    sessionRootObject.AddComponent<GameSessionRoot>();
                LoginSceneController controller =
                    controllerObject.AddComponent<LoginSceneController>();
                FakeMetaSessionClient metaClient = new FakeMetaSessionClient
                {
                    AdmissionResult = MetaHttpResult<MetaAdmissionOutcome>.Success(
                        new MetaAdmissionOutcome(
                            new MetaQueueStatus(MetaQueueState.Admitted, 0),
                            new MetaAdmissionResult(
                                "game-token-1",
                                PlayerServerEndpoint.Default,
                                1_785_000_000_004L)),
                        200)
                };
                FakeGameSessionConnector connector =
                    new FakeGameSessionConnector(PlayerNetworkSessionStatus.Connected);
                FakeLoginSceneLoader sceneLoader = new FakeLoginSceneLoader();
                controller.SessionRoot = sessionRoot;
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.MetaSessionClient = metaClient;
                controller.GameSessionConnector = connector;
                controller.SceneLoader = sceneLoader;
                controller.InviteCodeInput = "portfolio-2026";

                Assert.True(await controller.RunMetaAdmissionFlowAsync());

                Assert.AreEqual("https://meta.example.com", metaClient.AdmissionMetaBaseUrl);
                Assert.AreEqual("portfolio-2026", metaClient.InviteCode);
                Assert.True(sessionRoot.HasAdmission);
                Assert.AreEqual("game-token-1", sessionRoot.GameSessionToken);
                Assert.True(connector.ConnectCalledAfterAdmissionStored);
                Assert.True(sceneLoader.LobbyLoaded);
                Assert.AreEqual("입장하기", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(controllerObject);
                Object.DestroyImmediate(sessionRootObject);
            }
        }

        [Test]
        public async Task MetaAdmissionFlowRequiresNicknameBeforeSendingAdmissionRequest()
        {
            GameObject controllerObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    controllerObject.AddComponent<LoginSceneController>();
                FakeMetaSessionClient metaClient = new FakeMetaSessionClient
                {
                    AdmissionResult = MetaHttpResult<MetaAdmissionOutcome>.Success(
                        new MetaAdmissionOutcome(
                            new MetaQueueStatus(MetaQueueState.Admitted, 0),
                            new MetaAdmissionResult(
                                "game-token-1",
                                PlayerServerEndpoint.Default,
                                1_785_000_000_004L)),
                        200)
                };
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.MetaSessionClient = metaClient;
                controller.InviteCodeInput = "portfolio-2026";
                controller.ApplyAuthenticatedMetaSession(
                    new MetaAuthSession(true, 7L, true, string.Empty));

                Assert.False(await controller.RunMetaAdmissionFlowAsync());

                Assert.AreEqual(string.Empty, metaClient.AdmissionMetaBaseUrl);
                Assert.AreEqual(string.Empty, metaClient.InviteCode);
                Assert.AreEqual(
                    LoginSceneController.NicknameRequiredMessage,
                    controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(controllerObject);
            }
        }

        [Test]
        public async Task MetaAdmissionFlowRequiresInviteCodeBeforeSendingAdmissionRequest()
        {
            GameObject controllerObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    controllerObject.AddComponent<LoginSceneController>();
                FakeMetaSessionClient metaClient = new FakeMetaSessionClient
                {
                    AdmissionResult = MetaHttpResult<MetaAdmissionOutcome>.Success(
                        new MetaAdmissionOutcome(
                            new MetaQueueStatus(MetaQueueState.Admitted, 0),
                            new MetaAdmissionResult(
                                "game-token-1",
                                PlayerServerEndpoint.Default,
                                1_785_000_000_004L)),
                        200)
                };
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.MetaSessionClient = metaClient;
                controller.InviteCodeInput = "   ";
                controller.ApplyAuthenticatedMetaSession(
                    new MetaAuthSession(true, 7L, false, "Ready26"));

                Assert.False(await controller.RunMetaAdmissionFlowAsync());

                Assert.AreEqual(string.Empty, metaClient.AdmissionMetaBaseUrl);
                Assert.AreEqual(string.Empty, metaClient.InviteCode);
                Assert.AreEqual(
                    LoginSceneController.InviteCodeRequiredMessage,
                    controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(controllerObject);
            }
        }

        [Test]
        public async Task QueuePollUsesQueueTokenAndConnectsWhenAdmitted()
        {
            GameObject sessionRootObject = new GameObject("GameSessionRoot");
            GameObject controllerObject = new GameObject("LoginSceneController");
            try
            {
                GameSessionRoot sessionRoot =
                    sessionRootObject.AddComponent<GameSessionRoot>();
                LoginSceneController controller =
                    controllerObject.AddComponent<LoginSceneController>();
                FakeMetaSessionClient metaClient = new FakeMetaSessionClient
                {
                    AdmissionResult = MetaHttpResult<MetaAdmissionOutcome>.Success(
                        new MetaAdmissionOutcome(
                            new MetaQueueStatus(
                                MetaQueueState.Queued,
                                3,
                                "queue-token-1"),
                            MetaAdmissionResult.Empty),
                        200),
                    QueueResult = MetaHttpResult<MetaAdmissionOutcome>.Success(
                        new MetaAdmissionOutcome(
                            new MetaQueueStatus(MetaQueueState.Admitted, 0),
                            new MetaAdmissionResult(
                                "queue-admitted-token",
                                PlayerServerEndpoint.Default,
                                1_785_000_000_005L)),
                        200)
                };
                FakeGameSessionConnector connector =
                    new FakeGameSessionConnector(PlayerNetworkSessionStatus.Connected);
                FakeLoginSceneLoader sceneLoader = new FakeLoginSceneLoader();
                controller.SessionRoot = sessionRoot;
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.MetaSessionClient = metaClient;
                controller.GameSessionConnector = connector;
                controller.SceneLoader = sceneLoader;
                controller.InviteCodeInput = "portfolio-2026";

                Assert.False(await controller.RunMetaAdmissionFlowAsync());
                Assert.AreEqual("대기열 3번째", controller.StatusText);
                Assert.AreEqual(5.0f, controller.NextQueuePollDelaySeconds);
                Assert.False(connector.ConnectCalled);

                Assert.True(await controller.RunQueueStatusPollAsync());

                Assert.AreEqual("queue-token-1", metaClient.QueueToken);
                Assert.AreEqual("queue-admitted-token", sessionRoot.GameSessionToken);
                Assert.True(connector.ConnectCalledAfterAdmissionStored);
                Assert.True(sceneLoader.LobbyLoaded);
            }
            finally
            {
                Object.DestroyImmediate(controllerObject);
                Object.DestroyImmediate(sessionRootObject);
            }
        }

        [Test]
        public async Task QueuePollingWaitsFiveSecondsBeforeStatusRequest()
        {
            GameObject sessionRootObject = new GameObject("GameSessionRoot");
            GameObject controllerObject = new GameObject("LoginSceneController");
            try
            {
                GameSessionRoot sessionRoot =
                    sessionRootObject.AddComponent<GameSessionRoot>();
                LoginSceneController controller =
                    controllerObject.AddComponent<LoginSceneController>();
                FakeMetaSessionClient metaClient = new FakeMetaSessionClient
                {
                    AdmissionResult = MetaHttpResult<MetaAdmissionOutcome>.Success(
                        new MetaAdmissionOutcome(
                            new MetaQueueStatus(
                                MetaQueueState.Queued,
                                3,
                                "queue-token-1"),
                            MetaAdmissionResult.Empty),
                        200),
                    QueueResult = MetaHttpResult<MetaAdmissionOutcome>.Success(
                        new MetaAdmissionOutcome(
                            new MetaQueueStatus(
                                MetaQueueState.Queued,
                                2,
                                "queue-token-1"),
                            MetaAdmissionResult.Empty),
                        200)
                };
                controller.SessionRoot = sessionRoot;
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.MetaSessionClient = metaClient;
                controller.InviteCodeInput = "portfolio-2026";

                Assert.False(await controller.RunMetaAdmissionFlowAsync());
                Assert.False(await controller.StepQueuePollingAsync(4.9f));
                Assert.AreEqual(string.Empty, metaClient.QueueToken);

                Assert.False(await controller.StepQueuePollingAsync(0.1f));

                Assert.AreEqual("queue-token-1", metaClient.QueueToken);
                Assert.AreEqual("대기열 2번째", controller.StatusText);
                Assert.AreEqual(5.0f, controller.NextQueuePollDelaySeconds);
            }
            finally
            {
                Object.DestroyImmediate(controllerObject);
                Object.DestroyImmediate(sessionRootObject);
            }
        }

        [Test]
        public async Task NicknameSubmitFlowUpdatesMetaSessionBeforeAdmission()
        {
            GameObject rootObject = new GameObject("LoginSceneController");
            try
            {
                LoginSceneController controller =
                    rootObject.AddComponent<LoginSceneController>();
                FakeMetaSessionClient metaClient = new FakeMetaSessionClient
                {
                    NicknameResult = MetaHttpResult<MetaAuthSession>.Success(
                        new MetaAuthSession(true, 7L, false, "Player7"),
                        200)
                };
                controller.MetaBaseUrl = "https://meta.example.com";
                controller.MetaSessionClient = metaClient;
                controller.ApplyAuthenticatedMetaSession(
                    new MetaAuthSession(true, 7L, true, string.Empty));

                Assert.True(await controller.RunNicknameSubmitFlowAsync("Player7"));

                Assert.AreEqual("https://meta.example.com", metaClient.NicknameMetaBaseUrl);
                Assert.AreEqual("Player7", metaClient.Nickname);
                Assert.AreEqual("입장하기", controller.StatusText);
            }
            finally
            {
                Object.DestroyImmediate(rootObject);
            }
        }


        private sealed class FakeStandaloneOAuthLoginFlow : IStandaloneOAuthLoginFlow
        {
            private readonly MetaHttpResult<MetaAuthSession> result;

            public FakeStandaloneOAuthLoginFlow(MetaHttpResult<MetaAuthSession> result)
            {
                this.result = result;
            }

            public string MetaBaseUrl { get; private set; }

            public Task<MetaHttpResult<MetaAuthSession>> LoginAsync(string metaBaseUrl)
            {
                MetaBaseUrl = metaBaseUrl;
                return Task.FromResult(result);
            }
        }

        private sealed class ThrowingStandaloneOAuthLoginFlow : IStandaloneOAuthLoginFlow
        {
            public Task<MetaHttpResult<MetaAuthSession>> LoginAsync(string metaBaseUrl)
            {
                throw new System.InvalidOperationException("oauth failed");
            }
        }

        private sealed class FakeMetaSessionClient : IMetaSessionClient
        {
            public MetaHttpResult<MetaNicknameAvailability> NicknameCheckResult { get; set; }
            public MetaHttpResult<MetaAdmissionOutcome> AdmissionResult { get; set; }
            public MetaHttpResult<MetaAdmissionOutcome> QueueResult { get; set; }
            public MetaHttpResult<MetaAuthSession> NicknameResult { get; set; }
            public string NicknameCheckMetaBaseUrl { get; private set; } = string.Empty;
            public string NicknameCheck { get; private set; } = string.Empty;
            public string AdmissionMetaBaseUrl { get; private set; } = string.Empty;
            public string InviteCode { get; private set; } = string.Empty;
            public string QueueToken { get; private set; } = string.Empty;
            public string NicknameMetaBaseUrl { get; private set; } = string.Empty;
            public string Nickname { get; private set; } = string.Empty;

            public Task<MetaHttpResult<MetaAuthSession>> ExchangeStandaloneCodeAsync(
                string metaBaseUrl,
                string code,
                string state)
            {
                return Task.FromResult(
                    MetaHttpResult<MetaAuthSession>.Failure(0, "not implemented"));
            }

            public Task<MetaHttpResult<MetaNicknameAvailability>> CheckNicknameAsync(
                string metaBaseUrl,
                string nickname)
            {
                NicknameCheckMetaBaseUrl = metaBaseUrl;
                NicknameCheck = nickname;
                return Task.FromResult(NicknameCheckResult);
            }

            public Task<MetaHttpResult<MetaAuthSession>> UpdateNicknameAsync(
                string metaBaseUrl,
                string nickname)
            {
                NicknameMetaBaseUrl = metaBaseUrl;
                Nickname = nickname;
                return Task.FromResult(NicknameResult);
            }

            public Task<MetaHttpResult<MetaAdmissionOutcome>> EnterAdmissionAsync(
                string metaBaseUrl,
                string inviteCode)
            {
                AdmissionMetaBaseUrl = metaBaseUrl;
                InviteCode = inviteCode;
                return Task.FromResult(AdmissionResult);
            }

            public Task<MetaHttpResult<MetaAdmissionOutcome>> GetQueueStatusAsync(
                string metaBaseUrl,
                string queueToken)
            {
                QueueToken = queueToken;
                return Task.FromResult(QueueResult);
            }
        }

        private sealed class FakeGameSessionConnector : ILoginGameSessionConnector
        {
            private readonly PlayerNetworkSessionStatus status;

            public FakeGameSessionConnector(PlayerNetworkSessionStatus status)
            {
                this.status = status;
            }

            public bool ConnectCalledAfterAdmissionStored { get; private set; }
            public bool ConnectCalled { get; private set; }

            public Task<PlayerNetworkSessionStatus> ConnectAsync(GameSessionRoot root)
            {
                ConnectCalled = true;
                ConnectCalledAfterAdmissionStored = root != null && root.HasAdmission;
                return Task.FromResult(status);
            }
        }

        private sealed class FakeLoginSceneLoader : ILoginSceneLoader
        {
            public bool LobbyLoaded { get; private set; }

            public void LoadLobbyScene()
            {
                LobbyLoaded = true;
            }
        }
    }
}
