using System.Threading.Tasks;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.PlayerClient
{
    [DisallowMultipleComponent]
    public sealed class ArenaSceneController : MonoBehaviour
    {
        public const string WaitingOverlayText = "다른 플레이어 로딩 중...";
        public const string FinalRankingTitleText = "최종 순위";
        public const string LobbyReturnButtonText = "로비로 이동";
        public const float FinalRankingOverlayWidth = 480.0f;
        public const float FinalRankingOverlayHeight = 260.0f;

        [SerializeField]
        private PlayerNetworkSessionController networkSessionController;

        private uint waitingRoomId;
        private ulong waitingBattleInstanceId;
        private bool waitingOverlayVisible;
        private bool gameplayInputEnabled;
        private bool sessionBoundaryFailed;
        private bool localLoadCompleteSent;
        private bool finalRankingOverlayVisible;
        private bool lobbyReturnButtonEnabled;
        private bool finalResultCaptureInFlight;
        private PlayerBattleFinalRanking finalRanking;

        public PlayerNetworkSessionController NetworkSessionController
        {
            get => networkSessionController;
            set => networkSessionController = value;
        }

        public bool WaitingOverlayVisible => waitingOverlayVisible;

        public string WaitingText => waitingOverlayVisible ? WaitingOverlayText : string.Empty;

        public bool GameplayInputEnabled => gameplayInputEnabled;

        public bool SessionBoundaryFailed => sessionBoundaryFailed;

        public bool LocalLoadCompleteSent => localLoadCompleteSent;

        public bool FinalRankingOverlayVisible => finalRankingOverlayVisible;

        public string FinalRankingTitle =>
            finalRankingOverlayVisible ? FinalRankingTitleText : string.Empty;

        public bool LobbyReturnButtonEnabled => lobbyReturnButtonEnabled;

        public PlayerBattleFinalRankingRow[] VisibleFinalRankingRows =>
            finalRankingOverlayVisible ?
                finalRanking.ToArray() :
                new PlayerBattleFinalRankingRow[0];

        public static Rect CalculateFinalRankingOverlayRect(int screenWidth, int screenHeight)
        {
            float scale = PlayerManualGuiScaler.CalculateScale(screenHeight);
            float scaledScreenWidth = Mathf.Max(0.0f, screenWidth / scale);
            float scaledScreenHeight = Mathf.Max(0.0f, screenHeight / scale);
            float x = Mathf.Max(
                16.0f,
                (scaledScreenWidth - FinalRankingOverlayWidth) * 0.5f);
            float y = Mathf.Max(
                16.0f,
                (scaledScreenHeight - FinalRankingOverlayHeight) * 0.5f);
            return new Rect(
                x,
                y,
                FinalRankingOverlayWidth,
                FinalRankingOverlayHeight);
        }

        public void EnterPreStartWaiting(uint roomId, ulong battleInstanceId)
        {
            if (roomId == 0U || battleInstanceId == 0UL)
            {
                MarkSessionBoundaryFailed();
                return;
            }

            waitingRoomId = roomId;
            waitingBattleInstanceId = battleInstanceId;
            waitingOverlayVisible = true;
            gameplayInputEnabled = false;
            sessionBoundaryFailed = false;
            localLoadCompleteSent = false;
            finalRankingOverlayVisible = false;
            lobbyReturnButtonEnabled = false;
            finalRanking = default;
        }

        public async Task<bool> CompleteLocalLoadAsync()
        {
            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            if (controller == null || waitingRoomId == 0U || waitingBattleInstanceId == 0UL)
            {
                return false;
            }

            bool sent = await controller.SendArenaLoadCompleteAsync();
            if (!sent)
            {
                return false;
            }

            localLoadCompleteSent = true;
            bool started = await controller.CaptureArenaGameplayStartAsync();
            if (!started)
            {
                MarkSessionBoundaryFailed();
                return false;
            }

            ApplyArenaGameplayStart(
                controller.ArenaGameplayStartRoomId,
                controller.ArenaGameplayStartBattleInstanceId);
            return gameplayInputEnabled;
        }

        public void ApplyArenaGameplayStart(uint roomId, ulong battleInstanceId)
        {
            if (roomId == 0U ||
                battleInstanceId == 0UL ||
                roomId != waitingRoomId ||
                battleInstanceId != waitingBattleInstanceId)
            {
                MarkSessionBoundaryFailed();
                return;
            }

            waitingOverlayVisible = false;
            gameplayInputEnabled = true;
            sessionBoundaryFailed = false;
            _ = CaptureFinalResultAsync();
        }

        public void ApplyBattleFinalRanking(PlayerBattleFinalRanking ranking)
        {
            if (ranking.RoomId == 0U ||
                ranking.BattleInstanceId == 0UL ||
                ranking.Count == 0 ||
                ranking.RoomId != waitingRoomId ||
                ranking.BattleInstanceId != waitingBattleInstanceId)
            {
                MarkSessionBoundaryFailed();
                return;
            }

            finalRanking = ranking;
            finalRankingOverlayVisible = true;
            lobbyReturnButtonEnabled = false;
            waitingOverlayVisible = false;
            gameplayInputEnabled = false;
            sessionBoundaryFailed = false;
        }

        public void ApplyLobbyReturnVisibility(
            uint previousRoomId,
            PlayerLobbyReturnReason reason)
        {
            if (!finalRankingOverlayVisible ||
                finalRanking.RoomId != previousRoomId ||
                reason != PlayerLobbyReturnReason.None)
            {
                MarkSessionBoundaryFailed();
                return;
            }

            lobbyReturnButtonEnabled = true;
            gameplayInputEnabled = false;
            sessionBoundaryFailed = false;
        }

        private void Start()
        {
            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            if (controller != null &&
                controller.BattleLoadEntryCaptured &&
                controller.BattleLoadEntryRoomId != 0U &&
                controller.BattleInstanceId != 0UL)
            {
                EnterPreStartWaiting(controller.BattleLoadEntryRoomId, controller.BattleInstanceId);
                _ = CompleteLocalLoadAsync();
            }
        }

        private PlayerNetworkSessionController ResolveNetworkSessionController()
        {
            if (networkSessionController != null)
            {
                return networkSessionController;
            }

            networkSessionController = FindAnyObjectByType<PlayerNetworkSessionController>();
            return networkSessionController;
        }

        private async Task CaptureFinalResultAsync()
        {
            if (finalResultCaptureInFlight)
            {
                return;
            }

            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            if (controller == null)
            {
                return;
            }

            finalResultCaptureInFlight = true;
            try
            {
                bool rankingCaptured =
                    controller.BattleFinalRankingCaptured ||
                    await controller.CaptureBattleFinalRankingAsync();
                if (!rankingCaptured)
                {
                    MarkSessionBoundaryFailed();
                    return;
                }

                ApplyBattleFinalRanking(controller.BattleFinalRanking);

                bool lobbyReturnCaptured =
                    controller.LobbyReturnCaptured ||
                    await controller.CaptureLobbyReturnVisibilityAsync();
                if (!lobbyReturnCaptured)
                {
                    MarkSessionBoundaryFailed();
                    return;
                }

                ApplyLobbyReturnVisibility(
                    controller.LobbyReturnPreviousRoomId,
                    controller.LobbyReturnReason);
            }
            finally
            {
                finalResultCaptureInFlight = false;
            }
        }

        private void MarkSessionBoundaryFailed()
        {
            waitingOverlayVisible = true;
            gameplayInputEnabled = false;
            sessionBoundaryFailed = true;
            finalRankingOverlayVisible = false;
            lobbyReturnButtonEnabled = false;
            finalRanking = default;
        }

        private void LoadLobbyScene()
        {
            if (lobbyReturnButtonEnabled)
            {
                SceneManager.LoadScene("LobbyScene");
            }
        }

        private void OnGUI()
        {
            using (PlayerManualGuiScaler.Begin(Screen.height))
            {
                if (waitingOverlayVisible)
                {
                    GUILayout.BeginArea(new Rect(24.0f, 24.0f, 420.0f, 120.0f), GUI.skin.box);
                    GUILayout.Label(WaitingText);
                    GUILayout.EndArea();
                }

                if (finalRankingOverlayVisible)
                {
                    GUILayout.BeginArea(
                        CalculateFinalRankingOverlayRect(Screen.width, Screen.height),
                        GUI.skin.box);
                    GUILayout.Label(FinalRankingTitleText);
                    PlayerBattleFinalRankingRow[] rows = finalRanking.ToArray();
                    for (int index = 0; index < rows.Length; ++index)
                    {
                        PlayerBattleFinalRankingRow row = rows[index];
                        GUILayout.Label(
                            $"{row.Rank}위  {row.Nickname}  {row.TotalAssetValue}");
                    }

                    GUI.enabled = lobbyReturnButtonEnabled;
                    if (GUILayout.Button(LobbyReturnButtonText))
                    {
                        LoadLobbyScene();
                    }
                    GUI.enabled = true;
                    GUILayout.EndArea();
                }
            }
        }
    }
}
