using System;
using System.Threading.Tasks;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.PlayerClient
{
    public interface IStandaloneOAuthLoginFlow
    {
        Task<MetaHttpResult<MetaAuthSession>> LoginAsync(string metaBaseUrl);
    }

    public interface ILoginGameSessionConnector
    {
        Task<PlayerNetworkSessionStatus> ConnectAsync(GameSessionRoot root);
    }

    public interface ILoginSceneLoader
    {
        void LoadLobbyScene();
    }

    [DisallowMultipleComponent]
    public sealed class LoginSceneController : MonoBehaviour
    {
        public const float QueuePollingIntervalSeconds = 5.0f;
        public const string GameTitleLabel = "Loot of Legends";
        public const string GoogleLoginLabel = "Google로 로그인";
        public const string PlayerNameLabel = "플레이어 이름";
        public const string NicknameSubmitLabel = PlayerNameLabel;
        public const string StartPlayerNameSetupLabel = "처음 시작하기";
        public const string CheckPlayerNameLabel = "중복 확인";
        public const string GoogleAuthenticatePlayerNameLabel = "Google로 인증";
        public const string GoogleAuthCompleteLabel = "Google 인증 완료";
        public const string CreatePlayerNameLabel = "생성";
        public const string CancelLabel = "취소";
        public const string InviteCodeLabel = "초대 코드";
        public const string AdmissionLabel = "입장하기";
        public const string QueueCheckingMessage = "대기열 상태를 확인하고 있습니다";
        public const string InviteCodeRequiredMessage = "초대 코드를 입력하세요";
        public const string AdmissionFailureMessage = "입장에 실패했습니다. 다시 시도해주세요";
        public const string GameServerReadinessFailureMessage =
            "게임 서버 연결 준비에 실패했습니다. 잠시 후 다시 시도해주세요.";
        public const string StandaloneOAuthHandoffFailureMessage =
            "Google 로그인 연결에 실패했습니다. 다시 시도해주세요";
        public const string NicknameRequiredMessage = "플레이어 이름 설정이 필요합니다";
        public const string LocalDevAdmissionMessage = "로컬 개발 입장이 준비되었습니다";
        public const string GameServerConnectingMessage = "게임 서버에 연결하고 있습니다";
        public const string LocalDevTokenPrefix = "dev-session:";
        public const long LocalDevReservationTtlMilliseconds = 60000L;
        public const float WelcomePanelWidth = 520.0f;
        public const float WelcomePanelHeight = 440.0f;
        public const float PlayerNameModalWidth = 420.0f;
        public const float PlayerNameModalHeight = 340.0f;
        public static readonly Color PlayerNameModalBackdropColor =
            new Color(0.0f, 0.0f, 0.0f, 0.72f);
        public static readonly Color PlayerNameModalPanelColor =
            new Color(0.12f, 0.16f, 0.20f, 1.0f);
        public static readonly Rect LoginPanelRect =
            new Rect(0.0f, 0.0f, WelcomePanelWidth, WelcomePanelHeight);

        [SerializeField]
        private GameSessionRoot sessionRoot;

        [SerializeField]
        private string nicknameInput = string.Empty;

        [SerializeField]
        private string inviteCodeInput = string.Empty;

        [SerializeField]
        private string metaBaseUrl = Release0ClientConfig.DefaultMetaBaseUrl;

        private string statusText = QueueCheckingMessage;
        private float nextQueuePollDelaySeconds;
        private bool shouldLoadLobbyAfterAdmission;
        private bool admissionInFlight;
        private bool nicknameRequiredForAdmission;
        private bool playerNameModalVisible;
        private bool playerNameGoogleAuthenticated;
        private string playerNameModalStatusText = string.Empty;
        private IStandaloneOAuthLoginFlow standaloneOAuthLoginFlow;
        private IMetaSessionClient metaSessionClient;
        private ILoginGameSessionConnector gameSessionConnector;
        private ILoginSceneLoader sceneLoader;
        private string activeQueueToken = string.Empty;
        private string checkedAvailablePlayerName = string.Empty;

        public string GameTitleText => GameTitleLabel;
        public string GoogleLoginButtonText => GoogleLoginLabel;
        public string NicknameSubmitButtonText => NicknameSubmitLabel;
        public string PlayerNameLabelText => PlayerNameLabel;
        public string StartPlayerNameSetupButtonText => StartPlayerNameSetupLabel;
        public string CheckPlayerNameButtonText => CheckPlayerNameLabel;
        public string GoogleAuthCompleteButtonText => GoogleAuthCompleteLabel;
        public string CreatePlayerNameButtonText => CreatePlayerNameLabel;
        public string InviteCodeLabelText => InviteCodeLabel;
        public string AdmissionButtonText => AdmissionLabel;
        public string StatusText => statusText;
        public float NextQueuePollDelaySeconds => nextQueuePollDelaySeconds;
        public bool PollsGameRoomListBeforeAdmission => false;
        public bool ShouldLoadLobbyAfterAdmission => shouldLoadLobbyAfterAdmission;
        public bool PlayerNameModalVisible => playerNameModalVisible;
        public bool WelcomePanelInputEnabled => !playerNameModalVisible;
        public string PlayerNameModalStatusText => playerNameModalStatusText;

        public string MetaBaseUrl
        {
            get => metaBaseUrl;
            set => metaBaseUrl = value ?? string.Empty;
        }

        public string InviteCodeInput
        {
            get => inviteCodeInput;
            set => inviteCodeInput = value ?? string.Empty;
        }

        public string PlayerNameInput
        {
            get => nicknameInput;
            set
            {
                string next = value ?? string.Empty;
                if (string.Equals(nicknameInput, next, StringComparison.Ordinal))
                {
                    return;
                }

                nicknameInput = next;
                ClearPlayerNameReadiness();
            }
        }

        public bool CanCheckPlayerName =>
            !admissionInFlight &&
            TryValidatePlayerName(nicknameInput, out _);

        public bool CanAuthenticatePlayerName =>
            !admissionInFlight &&
            !playerNameGoogleAuthenticated &&
            IsCurrentPlayerNameCheckedAvailable();

        public bool CanCreatePlayerName =>
            !admissionInFlight &&
            playerNameGoogleAuthenticated &&
            IsCurrentPlayerNameCheckedAvailable();

        public IStandaloneOAuthLoginFlow StandaloneOAuthLoginFlow
        {
            get => standaloneOAuthLoginFlow;
            set => standaloneOAuthLoginFlow = value;
        }

        public IMetaSessionClient MetaSessionClient
        {
            get => metaSessionClient;
            set => metaSessionClient = value;
        }

        public ILoginGameSessionConnector GameSessionConnector
        {
            get => gameSessionConnector;
            set => gameSessionConnector = value;
        }

        public ILoginSceneLoader SceneLoader
        {
            get => sceneLoader;
            set => sceneLoader = value;
        }

        public GameSessionRoot SessionRoot
        {
            get => sessionRoot;
            set => sessionRoot = value;
        }

        public void LoadRelease0ClientConfigFrom(string streamingAssetsPath)
        {
            metaBaseUrl = Release0ClientConfig.LoadMetaBaseUrl(
                streamingAssetsPath,
                metaBaseUrl);
        }

        public void ApplyQueueStatus(MetaQueueStatus status)
        {
            switch (status.State)
            {
                case MetaQueueState.Queued:
                    statusText = status.Position > 0 ?
                        $"대기열 {status.Position}번째" :
                        QueueCheckingMessage;
                    nextQueuePollDelaySeconds = QueuePollingIntervalSeconds;
                    break;
                case MetaQueueState.Admitted:
                    statusText = AdmissionLabel;
                    nextQueuePollDelaySeconds = 0.0f;
                    break;
                case MetaQueueState.GameServerReadinessFailed:
                    statusText = GameServerReadinessFailureMessage;
                    nextQueuePollDelaySeconds = 0.0f;
                    break;
                case MetaQueueState.QueueExpired:
                case MetaQueueState.NotQueued:
                    statusText = AdmissionFailureMessage;
                    nextQueuePollDelaySeconds = 0.0f;
                    break;
                default:
                    statusText = AdmissionFailureMessage;
                    nextQueuePollDelaySeconds = 0.0f;
                    break;
            }
        }

        public bool ApplyAdmissionResult(
            MetaQueueStatus status,
            MetaAdmissionResult admission)
        {
            ApplyQueueStatus(status);
            if (status.State != MetaQueueState.Admitted)
            {
                return false;
            }

            GameSessionRoot root = ResolveSessionRoot();
            if (root == null || !admission.IsValid)
            {
                statusText = AdmissionFailureMessage;
                return false;
            }

            root.StoreAdmission(admission);
            statusText = AdmissionLabel;
            shouldLoadLobbyAfterAdmission = false;
            return true;
        }

        public void ApplyStandaloneOAuthHandoffFailure()
        {
            statusText = StandaloneOAuthHandoffFailureMessage;
            nextQueuePollDelaySeconds = 0.0f;
            shouldLoadLobbyAfterAdmission = false;
            admissionInFlight = false;
            nicknameRequiredForAdmission = false;
        }

        public async Task<bool> RunStandaloneOAuthLoginFlowAsync()
        {
            if (admissionInFlight)
            {
                return false;
            }

            IStandaloneOAuthLoginFlow flow = ResolveStandaloneOAuthLoginFlow();
            admissionInFlight = true;
            statusText = QueueCheckingMessage;
            MetaHttpResult<MetaAuthSession> result;
            try
            {
                result = await flow.LoginAsync(metaBaseUrl);
            }
            finally
            {
                admissionInFlight = false;
            }

            if (!result.Succeeded)
            {
                ApplyStandaloneOAuthHandoffFailure();
                return false;
            }

            ApplyAuthenticatedMetaSession(result.Value);
            return result.Value.Authenticated;
        }

        public async Task<bool> RunMetaAdmissionFlowAsync()
        {
            if (admissionInFlight)
            {
                return false;
            }

            if (string.IsNullOrWhiteSpace(inviteCodeInput))
            {
                statusText = InviteCodeRequiredMessage;
                nextQueuePollDelaySeconds = 0.0f;
                return false;
            }

            if (nicknameRequiredForAdmission)
            {
                statusText = NicknameRequiredMessage;
                nextQueuePollDelaySeconds = 0.0f;
                return false;
            }

            admissionInFlight = true;
            statusText = QueueCheckingMessage;
            MetaHttpResult<MetaAdmissionOutcome> result =
                await ResolveMetaSessionClient().EnterAdmissionAsync(
                    metaBaseUrl,
                    inviteCodeInput);
            admissionInFlight = false;
            if (!result.Succeeded)
            {
                ApplyAdmissionFailure();
                return false;
            }

            return await ApplyAdmissionOutcomeAsync(result.Value);
        }

        public async Task<bool> RunQueueStatusPollAsync()
        {
            if (admissionInFlight)
            {
                return false;
            }

            if (string.IsNullOrEmpty(activeQueueToken))
            {
                ApplyAdmissionFailure();
                return false;
            }

            admissionInFlight = true;
            MetaHttpResult<MetaAdmissionOutcome> result =
                await ResolveMetaSessionClient().GetQueueStatusAsync(
                    metaBaseUrl,
                    activeQueueToken);
            admissionInFlight = false;
            if (!result.Succeeded)
            {
                ApplyAdmissionFailure();
                return false;
            }

            return await ApplyAdmissionOutcomeAsync(result.Value);
        }

        public async Task<bool> StepQueuePollingAsync(float deltaSeconds)
        {
            if (string.IsNullOrEmpty(activeQueueToken))
            {
                return false;
            }

            if (nextQueuePollDelaySeconds > 0.0f)
            {
                nextQueuePollDelaySeconds = Math.Max(
                    0.0f,
                    nextQueuePollDelaySeconds - Math.Max(0.0f, deltaSeconds));
                if (nextQueuePollDelaySeconds > 0.0f)
                {
                    return false;
                }
            }

            return await RunQueueStatusPollAsync();
        }

        public async Task<bool> RunNicknameSubmitFlowAsync(string nickname)
        {
            if (admissionInFlight)
            {
                return false;
            }

            if (string.IsNullOrWhiteSpace(nickname))
            {
                ApplyNicknameValidationFailure(MetaNicknameValidationFailure.Missing);
                return false;
            }

            admissionInFlight = true;
            statusText = QueueCheckingMessage;
            MetaHttpResult<MetaAuthSession> result =
                await ResolveMetaSessionClient().UpdateNicknameAsync(
                    metaBaseUrl,
                    nickname);
            admissionInFlight = false;
            if (!result.Succeeded)
            {
                ApplyAdmissionFailure();
                return false;
            }

            ApplyAuthenticatedMetaSession(result.Value);
            return result.Value.Authenticated && !result.Value.NicknameRequired;
        }

        public void OpenPlayerNameModal()
        {
            nicknameInput = string.Empty;
            ClearPlayerNameReadiness();
            playerNameModalVisible = true;
        }

        public void ClosePlayerNameModal()
        {
            playerNameModalVisible = false;
            nicknameInput = string.Empty;
            ClearPlayerNameReadiness();
            playerNameModalStatusText = string.Empty;
        }

        public async Task<bool> RunPlayerNameCheckFlowAsync()
        {
            if (admissionInFlight)
            {
                return false;
            }

            if (!TryValidatePlayerName(nicknameInput, out MetaNicknameValidationFailure failure))
            {
                playerNameModalStatusText = MetaApiClient.NicknameFailureMessage(failure);
                return false;
            }

            string playerName = nicknameInput.Trim();
            admissionInFlight = true;
            playerNameModalStatusText = QueueCheckingMessage;
            MetaHttpResult<MetaNicknameAvailability> result =
                await ResolveMetaSessionClient().CheckNicknameAsync(
                    metaBaseUrl,
                    playerName);
            admissionInFlight = false;
            if (!result.Succeeded)
            {
                playerNameModalStatusText = AdmissionFailureMessage;
                return false;
            }

            if (!result.Value.Available)
            {
                ClearPlayerNameReadiness();
                playerNameModalStatusText = string.IsNullOrWhiteSpace(result.Value.Message) ?
                    MetaApiClient.NicknameFailureMessage(MetaNicknameValidationFailure.Duplicate) :
                    result.Value.Message;
                return false;
            }

            checkedAvailablePlayerName = playerName;
            playerNameGoogleAuthenticated = false;
            playerNameModalStatusText = string.IsNullOrWhiteSpace(result.Value.Message) ?
                "사용 가능한 플레이어 이름입니다" :
                result.Value.Message;
            return true;
        }

        public async Task<bool> RunPlayerNameGoogleAuthFlowAsync()
        {
            if (!CanAuthenticatePlayerName)
            {
                return false;
            }

            if (!await RunStandaloneOAuthLoginFlowAsync())
            {
                playerNameGoogleAuthenticated = false;
                return false;
            }

            if (!nicknameRequiredForAdmission)
            {
                playerNameModalVisible = false;
                ClearPlayerNameReadiness();
                return true;
            }

            playerNameGoogleAuthenticated = true;
            playerNameModalStatusText = GoogleAuthCompleteLabel;
            return true;
        }

        public async Task<bool> RunPlayerNameCreateFlowAsync(string playerName)
        {
            if (!string.Equals(playerName ?? string.Empty, nicknameInput, StringComparison.Ordinal))
            {
                PlayerNameInput = playerName;
            }

            if (!CanCreatePlayerName)
            {
                return false;
            }

            bool created = await RunNicknameSubmitFlowAsync(nicknameInput.Trim());
            if (created)
            {
                playerNameModalVisible = false;
                nicknameInput = string.Empty;
                ClearPlayerNameReadiness();
            }

            return created;
        }

        public void ApplyAuthenticatedMetaSession(MetaAuthSession session)
        {
            shouldLoadLobbyAfterAdmission = false;
            nextQueuePollDelaySeconds = 0.0f;
            admissionInFlight = false;
            nicknameRequiredForAdmission = session.Authenticated &&
                session.NicknameRequired;

            if (!session.Authenticated)
            {
                ApplyStandaloneOAuthHandoffFailure();
                return;
            }

            statusText = session.NicknameRequired ?
                NicknameRequiredMessage :
                AdmissionLabel;
        }

        public void ApplyNicknameValidationFailure(MetaNicknameValidationFailure failure)
        {
            statusText = MetaApiClient.NicknameFailureMessage(failure);
            nextQueuePollDelaySeconds = 0.0f;
        }

        public void ApplyAdmissionFailure()
        {
            statusText = AdmissionFailureMessage;
            nextQueuePollDelaySeconds = 0.0f;
            shouldLoadLobbyAfterAdmission = false;
            admissionInFlight = false;
        }

        public bool TryStartLocalDevAdmission(string nickname)
        {
            shouldLoadLobbyAfterAdmission = false;
            if (!IsLocalDevAdmissionAvailable())
            {
                ApplyAdmissionFailure();
                return false;
            }

            if (!TryValidatePlayerName(nickname, out MetaNicknameValidationFailure failure))
            {
                ApplyNicknameValidationFailure(failure);
                return false;
            }

            GameSessionRoot root = ResolveSessionRoot();
            if (root == null)
            {
                ApplyAdmissionFailure();
                return false;
            }

            root.StoreAdmission(
                new MetaAdmissionResult(
                    LocalDevTokenPrefix + nickname,
                    PlayerServerEndpoint.Default,
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() +
                        LocalDevReservationTtlMilliseconds));
            statusText = LocalDevAdmissionMessage;
            nextQueuePollDelaySeconds = 0.0f;
            return true;
        }

        public async Task<bool> RunLocalDevAdmissionFlowAsync(string nickname)
        {
            if (admissionInFlight)
            {
                return false;
            }

            if (!TryStartLocalDevAdmission(nickname))
            {
                return false;
            }

            GameSessionRoot root = ResolveSessionRoot();
            PlayerNetworkSessionController controller = EnsureNetworkSessionController(root);
            if (controller == null)
            {
                ApplyAdmissionFailure();
                root?.ClearAdmission();
                return false;
            }

            admissionInFlight = true;
            statusText = GameServerConnectingMessage;
            await controller.ConnectAsync();
            if (controller.Status != PlayerNetworkSessionStatus.Connected)
            {
                ApplyAdmissionFailure();
                root.ClearAdmission();
                return false;
            }

            admissionInFlight = false;
            shouldLoadLobbyAfterAdmission = true;
            statusText = AdmissionLabel;
            SceneManager.LoadScene(PlayerClientBootstrap.LobbySceneName);
            return true;
        }

        public static bool IsLocalDevAdmissionAvailable()
        {
#if UNITY_EDITOR || DEVELOPMENT_BUILD
            return true;
#else
            return false;
#endif
        }

        private static bool TryValidatePlayerName(
            string nickname,
            out MetaNicknameValidationFailure failure)
        {
            if (string.IsNullOrWhiteSpace(nickname))
            {
                failure = MetaNicknameValidationFailure.Missing;
                return false;
            }

            string trimmed = nickname.Trim();
            if (trimmed.Length < 2 || trimmed.Length > 12)
            {
                failure = MetaNicknameValidationFailure.Length;
                return false;
            }

            for (int index = 0; index < trimmed.Length; ++index)
            {
                if (!char.IsLetterOrDigit(trimmed[index]))
                {
                    failure = MetaNicknameValidationFailure.UnsupportedCharacters;
                    return false;
                }
            }

            failure = default;
            return true;
        }

        private bool IsCurrentPlayerNameCheckedAvailable()
        {
            return !string.IsNullOrEmpty(checkedAvailablePlayerName) &&
                string.Equals(
                    checkedAvailablePlayerName,
                    nicknameInput.Trim(),
                    StringComparison.Ordinal);
        }

        private void ClearPlayerNameReadiness()
        {
            checkedAvailablePlayerName = string.Empty;
            playerNameGoogleAuthenticated = false;
            playerNameModalStatusText = string.Empty;
        }

        private GameSessionRoot ResolveSessionRoot()
        {
            if (sessionRoot != null)
            {
                return sessionRoot;
            }

            sessionRoot = UnityEngine.Object.FindAnyObjectByType<GameSessionRoot>();
            return sessionRoot;
        }

        private async Task<bool> ApplyAdmissionOutcomeAsync(
            MetaAdmissionOutcome outcome)
        {
            if (outcome.Status.State == MetaQueueState.Queued)
            {
                if (!string.IsNullOrEmpty(outcome.Status.QueueToken))
                {
                    activeQueueToken = outcome.Status.QueueToken;
                }

                ApplyQueueStatus(outcome.Status);
                return false;
            }

            activeQueueToken = string.Empty;
            if (!ApplyAdmissionResult(outcome.Status, outcome.Admission))
            {
                return false;
            }

            return await ConnectStoredAdmissionAndLoadLobbyAsync();
        }

        private async Task<bool> ConnectStoredAdmissionAndLoadLobbyAsync()
        {
            GameSessionRoot root = ResolveSessionRoot();
            if (root == null || !root.HasAdmission)
            {
                ApplyAdmissionFailure();
                return false;
            }

            admissionInFlight = true;
            statusText = GameServerConnectingMessage;
            PlayerNetworkSessionStatus connectStatus =
                await ResolveGameSessionConnector().ConnectAsync(root);
            admissionInFlight = false;
            if (connectStatus != PlayerNetworkSessionStatus.Connected)
            {
                root.ClearAdmission();
                ApplyAdmissionFailure();
                return false;
            }

            shouldLoadLobbyAfterAdmission = true;
            statusText = AdmissionLabel;
            ResolveSceneLoader().LoadLobbyScene();
            return true;
        }

        private IStandaloneOAuthLoginFlow ResolveStandaloneOAuthLoginFlow()
        {
            if (standaloneOAuthLoginFlow == null)
            {
                standaloneOAuthLoginFlow = new StandaloneOAuthLoginFlow(
                    new StandaloneOAuthLoopbackFactory(),
                    new UnityExternalUrlOpener(),
                    ResolveMetaSessionClient(),
                    LootOfLegends.PlayerClient.StandaloneOAuthLoginFlow
                        .DefaultCallbackTimeout);
            }

            return standaloneOAuthLoginFlow;
        }

        private IMetaSessionClient ResolveMetaSessionClient()
        {
            if (metaSessionClient == null)
            {
                metaSessionClient = new MetaHttpSessionClient();
            }

            return metaSessionClient;
        }

        private ILoginGameSessionConnector ResolveGameSessionConnector()
        {
            if (gameSessionConnector == null)
            {
                gameSessionConnector = new UnityLoginGameSessionConnector();
            }

            return gameSessionConnector;
        }

        private ILoginSceneLoader ResolveSceneLoader()
        {
            if (sceneLoader == null)
            {
                sceneLoader = new UnityLoginSceneLoader();
            }

            return sceneLoader;
        }

        private static PlayerNetworkSessionController EnsureNetworkSessionController(
            GameSessionRoot root)
        {
            PlayerNetworkSessionController controller =
                UnityEngine.Object.FindAnyObjectByType<PlayerNetworkSessionController>();
            if (controller != null)
            {
                return controller;
            }

            if (root == null)
            {
                return null;
            }

            return root.gameObject.AddComponent<PlayerNetworkSessionController>();
        }

        private void Update()
        {
            if (!string.IsNullOrEmpty(activeQueueToken) && !admissionInFlight)
            {
                _ = StepQueuePollingAsync(Time.unscaledDeltaTime);
            }
        }

        private void Awake()
        {
            LoadRelease0ClientConfigFrom(Application.streamingAssetsPath);
        }

        public static Rect CalculateWelcomePanelRect(int screenWidth, int screenHeight)
        {
            return PlayerManualGuiScaler.CalculateCenteredRect(
                screenWidth,
                screenHeight,
                WelcomePanelWidth,
                WelcomePanelHeight);
        }

        public static Rect CalculatePlayerNameModalRect(int screenWidth, int screenHeight)
        {
            return PlayerManualGuiScaler.CalculateCenteredRect(
                screenWidth,
                screenHeight,
                PlayerNameModalWidth,
                PlayerNameModalHeight);
        }

        private void OnGUI()
        {
            using (PlayerManualGuiScaler.Begin(Screen.height))
            {
                bool previousEnabled = GUI.enabled;
                GUI.enabled = previousEnabled && WelcomePanelInputEnabled;
                DrawWelcomePanel();
                GUI.enabled = previousEnabled;

                if (playerNameModalVisible)
                {
                    DrawModalBackdrop();
                    DrawPlayerNameModal();
                }
            }
        }

        private void DrawWelcomePanel()
        {
            GUILayout.BeginArea(
                CalculateWelcomePanelRect(Screen.width, Screen.height),
                GUI.skin.box);
            GUILayout.Label(GameTitleLabel);
            GUILayout.Label(statusText);

            if (GUILayout.Button(GoogleLoginLabel))
            {
                _ = RunStandaloneOAuthLoginFlowAsync();
            }

            if (GUILayout.Button(StartPlayerNameSetupLabel))
            {
                OpenPlayerNameModal();
            }

            GUILayout.Label(InviteCodeLabel);
            inviteCodeInput = GUILayout.TextField(inviteCodeInput, 32);
            if (GUILayout.Button(AdmissionLabel))
            {
                _ = RunMetaAdmissionFlowAsync();
            }

            GUILayout.EndArea();
        }

        private static Rect CalculateScaledFullScreenRect(int screenWidth, int screenHeight)
        {
            float scale = PlayerManualGuiScaler.CalculateScale(screenHeight);
            return new Rect(
                0.0f,
                0.0f,
                Mathf.Max(0.0f, screenWidth / scale),
                Mathf.Max(0.0f, screenHeight / scale));
        }

        private static void DrawSolidRect(Rect rect, Color color)
        {
            Color previousColor = GUI.color;
            GUI.color = color;
            GUI.DrawTexture(rect, Texture2D.whiteTexture);
            GUI.color = previousColor;
        }

        private void DrawModalBackdrop()
        {
            DrawSolidRect(
                CalculateScaledFullScreenRect(Screen.width, Screen.height),
                PlayerNameModalBackdropColor);
        }

        private void DrawPlayerNameModal()
        {
            Rect modalRect = CalculatePlayerNameModalRect(Screen.width, Screen.height);
            DrawSolidRect(modalRect, PlayerNameModalPanelColor);
            GUILayout.BeginArea(
                modalRect,
                GUI.skin.box);
            GUILayout.Label(PlayerNameLabel);
            PlayerNameInput = GUILayout.TextField(PlayerNameInput, 12);
            if (!string.IsNullOrEmpty(playerNameModalStatusText))
            {
                GUILayout.Label(playerNameModalStatusText);
            }

            bool previousEnabled = GUI.enabled;
            GUI.enabled = previousEnabled && CanCheckPlayerName;
            if (GUILayout.Button(CheckPlayerNameLabel))
            {
                _ = RunPlayerNameCheckFlowAsync();
            }

            GUI.enabled = previousEnabled && CanAuthenticatePlayerName;
            string googleAuthLabel = playerNameGoogleAuthenticated ?
                GoogleAuthCompleteLabel :
                GoogleAuthenticatePlayerNameLabel;
            if (GUILayout.Button(googleAuthLabel))
            {
                _ = RunPlayerNameGoogleAuthFlowAsync();
            }

            GUI.enabled = previousEnabled && CanCreatePlayerName;
            if (GUILayout.Button(CreatePlayerNameLabel))
            {
                _ = RunPlayerNameCreateFlowAsync(nicknameInput);
            }

            GUI.enabled = previousEnabled;
            if (GUILayout.Button(CancelLabel))
            {
                ClosePlayerNameModal();
            }

            GUILayout.EndArea();
        }
    }

    public sealed class UnityLoginGameSessionConnector :
        ILoginGameSessionConnector
    {
        public async Task<PlayerNetworkSessionStatus> ConnectAsync(GameSessionRoot root)
        {
            PlayerNetworkSessionController controller =
                UnityEngine.Object.FindAnyObjectByType<PlayerNetworkSessionController>();
            if (controller == null && root != null)
            {
                controller = root.gameObject.AddComponent<PlayerNetworkSessionController>();
            }

            if (controller == null)
            {
                return PlayerNetworkSessionStatus.Failed;
            }

            await controller.ConnectAsync();
            return controller.Status;
        }
    }

    public sealed class UnityLoginSceneLoader : ILoginSceneLoader
    {
        public void LoadLobbyScene()
        {
            SceneManager.LoadScene(PlayerClientBootstrap.LobbySceneName);
        }
    }
}
