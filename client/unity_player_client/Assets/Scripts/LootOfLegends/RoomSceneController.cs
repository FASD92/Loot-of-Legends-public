using System;
using System.Threading.Tasks;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.PlayerClient
{
    [DisallowMultipleComponent]
    public sealed class RoomSceneController : MonoBehaviour
    {
        public const string RoomLabel = "방";
        public const string HostPrefix = "방장";
        public const string ReadyLabel = "준비";
        public const string UnreadyLabel = "준비 취소";
        public const string LeaveLabel = "나가기";
        public const string HostStartBattleLabel = "배틀 시작";
        public const string HostKickLabel = "강퇴";
        public const string WaitingStatusText = "대기 중";
        public const string ReadyStatusText = "준비 완료";
        public const string CountCapacityPrefix = "현재 인원";
        public const string RoomInfoWaitingMessage = "방 정보를 기다리는 중";
        public const string ConnectionLostMessage = "서버 연결이 종료되었습니다";
        public const string HostKickReturnMessage = "강퇴당했습니다.";
        public const string LobbyReturnButtonText = "로비로 이동";
        public const float LobbyReturnModalWidth = 360.0f;
        public const float LobbyReturnModalHeight = 140.0f;

        [SerializeField]
        private PlayerNetworkSessionController networkSessionController;

        private MemberRowView[] memberRows = Array.Empty<MemberRowView>();
        private PlayerRoomTargetActionEntry[] targetActions =
            Array.Empty<PlayerRoomTargetActionEntry>();
        private ushort selfActionMask;
        private string roomTitle = string.Empty;
        private string hostText = $"{HostPrefix}: ";
        private string countCapacityText = $"{CountCapacityPrefix} 0/0";
        private string statusText = RoomInfoWaitingMessage;
        private bool lobbyReturnModalVisible;
        private string lobbyReturnModalMessage = string.Empty;
        private bool lobbySceneLoadRequested;
        private bool arenaSceneLoadRequested;
        private uint arenaSceneLoadRoomId;
        private ulong arenaSceneLoadBattleInstanceId;

        public PlayerNetworkSessionController NetworkSessionController
        {
            get => networkSessionController;
            set => networkSessionController = value;
        }

        public string RoomTitle => roomTitle;

        public string HostText => hostText;

        public string CountCapacityText => countCapacityText;

        public string StatusText => statusText;

        public bool LobbyReturnModalVisible => lobbyReturnModalVisible;

        public string LobbyReturnModalMessage => lobbyReturnModalMessage;

        public bool LobbySceneLoadRequested => lobbySceneLoadRequested;

        public string[] VisibleHeaderTexts
        {
            get
            {
                if (string.IsNullOrEmpty(roomTitle))
                {
                    return new[] { RoomLabel, countCapacityText, hostText };
                }

                return new[] { RoomLabel, roomTitle, countCapacityText, hostText };
            }
        }

        public bool ReadyVisible => HasSelfAction(PlayerTcpPacket.RoomActionReady);

        public bool UnreadyVisible => HasSelfAction(PlayerTcpPacket.RoomActionUnready);

        public bool LeaveVisible => HasSelfAction(PlayerTcpPacket.RoomActionLeaveRoom);

        public bool HostStartBattleVisible =>
            HasSelfAction(PlayerTcpPacket.RoomActionHostStartBattle);

        public bool LeaveRequestAvailable => LeaveVisible;

        public bool ArenaSceneLoadRequested => arenaSceneLoadRequested;

        public uint ArenaSceneLoadRoomId => arenaSceneLoadRoomId;

        public ulong ArenaSceneLoadBattleInstanceId => arenaSceneLoadBattleInstanceId;

        public MemberRowView[] VisibleMemberRows
        {
            get
            {
                if (memberRows.Length == 0)
                {
                    return Array.Empty<MemberRowView>();
                }

                MemberRowView[] copy = new MemberRowView[memberRows.Length];
                Array.Copy(memberRows, copy, memberRows.Length);
                return copy;
            }
        }

        public void ApplyRoomDetailState(PlayerRoomDetailState detail)
        {
            lobbyReturnModalVisible = false;
            lobbyReturnModalMessage = string.Empty;
            roomTitle = detail.Title;
            selfActionMask = detail.SelfActionMask;
            targetActions = detail.TargetActionsToArray();
            statusText = string.Empty;

            PlayerRoomMemberEntry[] members = detail.MembersToArray();
            memberRows = new MemberRowView[members.Length];
            countCapacityText = $"{CountCapacityPrefix} {members.Length}/{detail.MaxPlayers}";
            hostText = members.Length > 0 ?
                $"{HostPrefix}: {members[0].Nickname}" :
                $"{HostPrefix}: ";

            for (int index = 0; index < members.Length; ++index)
            {
                PlayerRoomMemberEntry member = members[index];
                memberRows[index] = new MemberRowView(
                    member.SessionId,
                    member.Nickname,
                    index == 0 ? HostPrefix : string.Empty,
                    member.Ready ? ReadyStatusText : WaitingStatusText,
                    HasTargetAction(member.SessionId, PlayerTcpPacket.TargetActionHostKick),
                    HostKickLabel);
            }
        }

        public static Rect CalculateLobbyReturnModalRect(int screenWidth, int screenHeight)
        {
            float scale = PlayerManualGuiScaler.CalculateScale(screenHeight);
            float scaledScreenWidth = Mathf.Max(0.0f, screenWidth / scale);
            float scaledScreenHeight = Mathf.Max(0.0f, screenHeight / scale);
            float x = Mathf.Max(
                16.0f,
                (scaledScreenWidth - LobbyReturnModalWidth) * 0.5f);
            float y = Mathf.Max(
                16.0f,
                (scaledScreenHeight - LobbyReturnModalHeight) * 0.5f);
            return new Rect(
                x,
                y,
                LobbyReturnModalWidth,
                LobbyReturnModalHeight);
        }

        public bool TryBuildHostKickRequest(ulong targetSessionId, out uint requestTargetSessionId)
        {
            requestTargetSessionId = 0U;
            if (targetSessionId == 0UL ||
                targetSessionId > uint.MaxValue ||
                !HasTargetAction(targetSessionId, PlayerTcpPacket.TargetActionHostKick))
            {
                return false;
            }

            requestTargetSessionId = (uint)targetSessionId;
            return true;
        }

        public Task<bool> RequestReadyAsync()
        {
            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            return ReadyVisible && controller != null ?
                controller.RequestReadyRoomAsync() :
                Task.FromResult(false);
        }

        public Task<bool> RequestUnreadyAsync()
        {
            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            return UnreadyVisible && controller != null ?
                controller.RequestUnreadyRoomAsync() :
                Task.FromResult(false);
        }

        public Task<bool> RequestHostStartBattleAsync()
        {
            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            return HostStartBattleVisible && controller != null ?
                controller.RequestHostStartBattleAsync() :
                Task.FromResult(false);
        }

        public async Task<bool> RequestLeaveAsync()
        {
            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            if (!LeaveRequestAvailable || controller == null)
            {
                return false;
            }

            bool left = await controller.RequestLeaveRoomAsync();
            if (left)
            {
                RequestLobbySceneLoad();
            }

            return left;
        }

        public Task<bool> RequestHostKickAsync(ulong targetSessionId)
        {
            if (!TryBuildHostKickRequest(targetSessionId, out uint requestTargetSessionId))
            {
                return Task.FromResult(false);
            }

            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            return controller != null ?
                controller.RequestHostKickAsync(requestTargetSessionId) :
                Task.FromResult(false);
        }

        public bool RequestArenaSceneLoad(uint roomId, ulong battleInstanceId)
        {
            if (roomId == 0U || battleInstanceId == 0UL)
            {
                return false;
            }

            arenaSceneLoadRequested = true;
            arenaSceneLoadRoomId = roomId;
            arenaSceneLoadBattleInstanceId = battleInstanceId;
            if (Application.isPlaying)
            {
                SceneManager.LoadScene(PlayerClientBootstrap.ArenaSceneName);
            }
            return true;
        }

        private void RequestLobbySceneLoad()
        {
            lobbySceneLoadRequested = true;
            LoadLobbyScene();
        }

        private void Update()
        {
            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            if (controller != null &&
                controller.BattleLoadEntryCaptured &&
                !arenaSceneLoadRequested)
            {
                RequestArenaSceneLoad(controller.BattleLoadEntryRoomId, controller.BattleInstanceId);
                return;
            }

            if (controller == null)
            {
                ApplyUnavailableRoomState(RoomInfoWaitingMessage, memberRows.Length == 0);
                return;
            }

            if (controller.LobbyReturnCaptured &&
                controller.LobbyReturnReason == PlayerLobbyReturnReason.HostKick &&
                controller.CurrentRoomId == 0U)
            {
                ApplyHostKickLobbyReturnState();
                return;
            }

            if (controller.Status != PlayerNetworkSessionStatus.Connected)
            {
                bool noRoomViewCaptured = memberRows.Length == 0 &&
                    (!controller.RoomDetailCaptured || controller.CurrentRoomId == 0U);
                ApplyUnavailableRoomState(
                    noRoomViewCaptured ? RoomInfoWaitingMessage : ConnectionLostMessage,
                    noRoomViewCaptured);
                return;
            }

            if (!controller.RoomDetailCaptured || controller.CurrentRoomId == 0U)
            {
                ApplyUnavailableRoomState(RoomInfoWaitingMessage, memberRows.Length == 0);
                return;
            }

            ApplyRoomDetailState(new PlayerRoomDetailState(
                controller.CurrentRoomId,
                controller.CurrentRoomStatus,
                controller.RoomTitle,
                (byte)Mathf.Clamp(controller.CurrentRoomMaxPlayers, 0, byte.MaxValue),
                controller.RoomMembers,
                controller.SelfRoomActionMask,
                controller.RoomTargetActions));
        }

        private void ApplyHostKickLobbyReturnState()
        {
            ClearRoomViewState();
            lobbyReturnModalVisible = true;
            lobbyReturnModalMessage = HostKickReturnMessage;
            statusText = HostKickReturnMessage;
        }

        private void ApplyUnavailableRoomState(string message, bool shouldClearView)
        {
            lobbyReturnModalVisible = false;
            lobbyReturnModalMessage = string.Empty;
            if (shouldClearView)
            {
                ClearRoomViewState();
            }

            selfActionMask = 0;
            targetActions = Array.Empty<PlayerRoomTargetActionEntry>();
            statusText = message ?? string.Empty;
        }

        private void ClearRoomViewState()
        {
            memberRows = Array.Empty<MemberRowView>();
            targetActions = Array.Empty<PlayerRoomTargetActionEntry>();
            selfActionMask = 0;
            roomTitle = string.Empty;
            hostText = $"{HostPrefix}: ";
            countCapacityText = $"{CountCapacityPrefix} 0/0";
        }

        private bool HasSelfAction(ushort actionMask)
        {
            return (selfActionMask & actionMask) == actionMask;
        }

        private bool HasTargetAction(ulong targetSessionId, ushort actionMask)
        {
            for (int index = 0; index < targetActions.Length; ++index)
            {
                PlayerRoomTargetActionEntry action = targetActions[index];
                if (action.TargetSessionId == targetSessionId &&
                    (action.TargetActionMask & actionMask) == actionMask)
                {
                    return true;
                }
            }

            return false;
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

        private void OnGUI()
        {
            using (PlayerManualGuiScaler.Begin(Screen.height))
            {
                if (lobbyReturnModalVisible)
                {
                    DrawLobbyReturnModal();
                    return;
                }

                GUILayout.BeginArea(new Rect(24.0f, 24.0f, 440.0f, 560.0f), GUI.skin.box);
                string[] headerTexts = VisibleHeaderTexts;
                for (int index = 0; index < headerTexts.Length; ++index)
                {
                    GUILayout.Label(headerTexts[index]);
                }

                if (!string.IsNullOrEmpty(statusText))
                {
                    GUILayout.Label(statusText);
                }

                bool wasEnabled = GUI.enabled;
                for (int index = 0; index < memberRows.Length; ++index)
                {
                    MemberRowView row = memberRows[index];
                    GUILayout.Label($"{row.Nickname} {row.RoleText} {row.ReadyText}");
                    if (row.KickVisible)
                    {
                        if (GUILayout.Button(row.KickButtonText))
                        {
                            _ = RequestHostKickAsync(row.SessionId);
                        }
                    }
                }

                if (ReadyVisible && GUILayout.Button(ReadyLabel))
                {
                    _ = RequestReadyAsync();
                }

                if (UnreadyVisible && GUILayout.Button(UnreadyLabel))
                {
                    _ = RequestUnreadyAsync();
                }

                if (LeaveVisible)
                {
                    GUI.enabled = wasEnabled && LeaveRequestAvailable;
                    if (GUILayout.Button(LeaveLabel))
                    {
                        _ = RequestLeaveAsync();
                    }

                    GUI.enabled = wasEnabled;
                }

                if (HostStartBattleVisible && GUILayout.Button(HostStartBattleLabel))
                {
                    _ = RequestHostStartBattleAsync();
                }

                GUI.enabled = wasEnabled;
                GUILayout.EndArea();
            }
        }

        private void DrawLobbyReturnModal()
        {
            GUILayout.BeginArea(
                CalculateLobbyReturnModalRect(Screen.width, Screen.height),
                GUI.skin.box);
            GUILayout.FlexibleSpace();
            GUILayout.Label(lobbyReturnModalMessage);
            if (GUILayout.Button(LobbyReturnButtonText))
            {
                LoadLobbyScene();
            }

            GUILayout.FlexibleSpace();
            GUILayout.EndArea();
        }

        private static void LoadLobbyScene()
        {
            if (Application.isPlaying)
            {
                SceneManager.LoadScene(PlayerClientBootstrap.LobbySceneName);
            }
        }

        public readonly struct MemberRowView
        {
            public readonly ulong SessionId;
            public readonly string Nickname;
            public readonly string RoleText;
            public readonly string ReadyText;
            public readonly bool KickVisible;
            public readonly string KickButtonText;

            public MemberRowView(
                ulong sessionId,
                string nickname,
                string roleText,
                string readyText,
                bool kickVisible,
                string kickButtonText)
            {
                SessionId = sessionId;
                Nickname = nickname ?? string.Empty;
                RoleText = roleText ?? string.Empty;
                ReadyText = readyText ?? string.Empty;
                KickVisible = kickVisible;
                KickButtonText = kickButtonText ?? string.Empty;
            }
        }
    }
}
