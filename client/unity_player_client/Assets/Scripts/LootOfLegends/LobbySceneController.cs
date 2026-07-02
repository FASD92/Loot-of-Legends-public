using System;
using System.Threading.Tasks;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.PlayerClient
{
    [DisallowMultipleComponent]
    public sealed class LobbySceneController : MonoBehaviour
    {
        public const float DelayedWarningThresholdSeconds = 3.0f;
        public const float DelayedWarningDurationSeconds = 10.0f;
        public const string LobbyLabel = "로비";
        public const string CreateRoomLabel = "방 만들기";
        public const string RoomTitleLabel = "방 제목";
        public const string MaxPlayersLabel = "최대 인원";
        public const string JoinRoomLabel = "방 입장";
        public const string EmptyRoomListMessage = "생성된 방이 없습니다";
        public const string InProgressStatusText = "게임중";
        public const string FullStatusText = "가득 참";
        public const string RefreshingRoomListMessage = "방 목록 갱신 중...";
        public const string RefreshRequiredMessage = "방 목록 갱신 후 이용할 수 있습니다";
        public const string RefreshDelayedWarningMessage = "방 목록 갱신이 지연되고 있습니다";
        public const string RoomEnteringMessage = "방에 입장하고 있습니다";
        public const string RoomCommandFailureMessage = "방 요청에 실패했습니다. 다시 시도해주세요";

        [SerializeField]
        private PlayerNetworkSessionController networkSessionController;

        [SerializeField]
        private string roomTitleInput = string.Empty;

        [SerializeField]
        private int maxPlayersInput = PlayerTcpPacket.CreateRoomMaxCapacity;

        private PlayerRoomListEntry[] currentServerRooms = Array.Empty<PlayerRoomListEntry>();
        private RoomRowView[] visibleRoomRows = Array.Empty<RoomRowView>();
        private string statusText = EmptyRoomListMessage;
        private bool roomListPending;
        private bool postReturnPending;
        private bool roomCommandInFlight;
        private bool roomCommandFailureVisible;
        private bool roomSceneLoadRequested;
        private float pendingElapsedSeconds;

        public PlayerNetworkSessionController NetworkSessionController
        {
            get => networkSessionController;
            set => networkSessionController = value;
        }

        public string StatusText => statusText;

        public bool CreateRoomEnabled =>
            CanBuildCreateRoomRequest && !roomCommandInFlight;

        public bool RoomSceneLoadRequested => roomSceneLoadRequested;

        public bool RefreshButtonVisible => false;

        public bool PollsRoomList => false;

        public string CreateRoomDraftTitle
        {
            get => roomTitleInput;
            set => roomTitleInput = value ?? string.Empty;
        }

        public int CreateRoomDraftCapacity
        {
            get => maxPlayersInput;
            set => maxPlayersInput = ClampCreateRoomCapacity(value);
        }

        public RoomRowView[] VisibleRoomRows
        {
            get
            {
                if (visibleRoomRows.Length == 0)
                {
                    return Array.Empty<RoomRowView>();
                }

                RoomRowView[] copy = new RoomRowView[visibleRoomRows.Length];
                Array.Copy(visibleRoomRows, copy, visibleRoomRows.Length);
                return copy;
            }
        }

        public void ApplyServerRoomList(PlayerRoomListEntry[] rooms)
        {
            currentServerRooms = CopyRooms(rooms);
            visibleRoomRows = BuildRows(currentServerRooms);
            roomListPending = false;
            postReturnPending = false;
            pendingElapsedSeconds = 0.0f;
            if (roomCommandFailureVisible && visibleRoomRows.Length == 0)
            {
                return;
            }

            roomCommandFailureVisible = false;
            statusText = visibleRoomRows.Length == 0 ? EmptyRoomListMessage : string.Empty;
        }

        public void ApplyServerRoomList(PlayerRoomListSnapshot snapshot)
        {
            ApplyServerRoomList(snapshot.ToArray());
        }

        public void ApplyPostReturnPending()
        {
            currentServerRooms = Array.Empty<PlayerRoomListEntry>();
            visibleRoomRows = Array.Empty<RoomRowView>();
            roomListPending = true;
            postReturnPending = true;
            pendingElapsedSeconds = 0.0f;
            roomCommandFailureVisible = false;
            statusText = RefreshRequiredMessage;
        }

        public void BeginRoomListRefresh()
        {
            currentServerRooms = Array.Empty<PlayerRoomListEntry>();
            visibleRoomRows = Array.Empty<RoomRowView>();
            roomListPending = true;
            postReturnPending = false;
            pendingElapsedSeconds = 0.0f;
            roomCommandFailureVisible = false;
            statusText = RefreshingRoomListMessage;
        }

        public void AdvanceRoomListRefreshDelay(float deltaSeconds)
        {
            if (!roomListPending || postReturnPending)
            {
                return;
            }

            pendingElapsedSeconds += Mathf.Max(0.0f, deltaSeconds);
            if (pendingElapsedSeconds >= DelayedWarningThresholdSeconds &&
                pendingElapsedSeconds <
                    DelayedWarningThresholdSeconds + DelayedWarningDurationSeconds)
            {
                statusText = RefreshDelayedWarningMessage;
                return;
            }

            statusText = RefreshingRoomListMessage;
        }

        public bool TryBuildJoinRoomRequest(int visibleRowIndex, out uint roomId)
        {
            roomId = 0U;
            if (visibleRowIndex < 0 || visibleRowIndex >= visibleRoomRows.Length)
            {
                return false;
            }

            RoomRowView row = visibleRoomRows[visibleRowIndex];
            if (!row.JoinEnabled)
            {
                return false;
            }

            roomId = row.RoomId;
            return roomId != 0U;
        }

        public bool TryBuildJoinRoomRequest(PlayerRoomListEntry serverRoom, out uint roomId)
        {
            roomId = 0U;
            if (roomListPending || postReturnPending || !IsJoinable(serverRoom))
            {
                return false;
            }

            for (int index = 0; index < currentServerRooms.Length; ++index)
            {
                PlayerRoomListEntry current = currentServerRooms[index];
                if (current.RoomId == serverRoom.RoomId &&
                    current.PlayerCount == serverRoom.PlayerCount &&
                    current.MaxPlayers == serverRoom.MaxPlayers &&
                    current.Status == serverRoom.Status)
                {
                    roomId = serverRoom.RoomId;
                    return true;
                }
            }

            return false;
        }

        public Task<bool> RequestJoinVisibleRoomAsync(int visibleRowIndex)
        {
            if (!TryBuildJoinRoomRequest(visibleRowIndex, out uint roomId))
            {
                return Task.FromResult(false);
            }

            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            return controller != null ?
                controller.RequestJoinRoomAsync(roomId) :
                Task.FromResult(false);
        }

        public Task<bool> RequestCreateRoomAsync()
        {
            if (!TryBuildCreateRoomRequest(out string roomTitle, out int maxPlayers))
            {
                return Task.FromResult(false);
            }

            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            return controller != null ?
                controller.RequestCreateRoomAsync(roomTitle, maxPlayers) :
                Task.FromResult(false);
        }

        public Task<bool> RunCreateRoomFlowAsync()
        {
            return RunRoomCommandFlowAsync(RequestCreateRoomAsync);
        }

        public Task<bool> RunJoinVisibleRoomFlowAsync(int visibleRowIndex)
        {
            return RunRoomCommandFlowAsync(() => RequestJoinVisibleRoomAsync(visibleRowIndex));
        }

        public bool TryBuildCreateRoomRequest(out string roomTitle, out int maxPlayers)
        {
            return TryBuildCreateRoomRequest(Input.compositionString, out roomTitle, out maxPlayers);
        }

        public bool TryBuildCreateRoomRequest(
            string compositionString,
            out string roomTitle,
            out int maxPlayers)
        {
            roomTitle = roomTitleInput ?? string.Empty;
            if (!string.IsNullOrEmpty(compositionString))
            {
                roomTitle += compositionString;
            }

            maxPlayers = ClampCreateRoomCapacity(maxPlayersInput);
            if (!CanBuildCreateRoomRequest)
            {
                return false;
            }

            try
            {
                PlayerTcpPacket.SerializeCreateRoomRequest(roomTitle, maxPlayers);
                return true;
            }
            catch (ArgumentException)
            {
                roomTitle = string.Empty;
                maxPlayers = 0;
                return false;
            }
        }

        private async Task<bool> RunRoomCommandFlowAsync(Func<Task<bool>> roomCommand)
        {
            if (roomCommandInFlight || roomSceneLoadRequested)
            {
                return false;
            }

            roomCommandInFlight = true;
            roomCommandFailureVisible = false;
            try
            {
                bool succeeded = await roomCommand();
                if (!succeeded)
                {
                    ApplyRoomCommandFailure(ResolveNetworkSessionController()?.LastError);
                    return false;
                }

                return RequestRoomSceneLoad();
            }
            catch (Exception ex)
            {
                ApplyRoomCommandFailure(ex.Message);
                return false;
            }
            finally
            {
                roomCommandInFlight = false;
            }
        }

        private void ApplyRoomCommandFailure(string detail)
        {
            roomCommandFailureVisible = true;
            statusText = string.IsNullOrEmpty(detail) ?
                RoomCommandFailureMessage :
                $"{RoomCommandFailureMessage} ({detail})";
            Debug.LogWarning(statusText);
        }

        private bool RequestRoomSceneLoad()
        {
            roomSceneLoadRequested = true;
            statusText = RoomEnteringMessage;
            if (Application.isPlaying)
            {
                SceneManager.LoadScene(PlayerClientBootstrap.RoomSceneName);
            }

            return true;
        }

        private bool CanBuildCreateRoomRequest =>
            !roomListPending && !postReturnPending && !roomSceneLoadRequested;

        private void Update()
        {
            if (roomCommandInFlight || roomSceneLoadRequested)
            {
                return;
            }

            if (roomListPending)
            {
                AdvanceRoomListRefreshDelay(Time.deltaTime);
                return;
            }

            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            if (controller != null)
            {
                ApplyServerRoomList(controller.RoomListEntries);
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

        private static PlayerRoomListEntry[] CopyRooms(PlayerRoomListEntry[] rooms)
        {
            if (rooms == null || rooms.Length == 0)
            {
                return Array.Empty<PlayerRoomListEntry>();
            }

            PlayerRoomListEntry[] copy = new PlayerRoomListEntry[rooms.Length];
            Array.Copy(rooms, copy, rooms.Length);
            return copy;
        }

        private static RoomRowView[] BuildRows(PlayerRoomListEntry[] rooms)
        {
            if (rooms == null || rooms.Length == 0)
            {
                return Array.Empty<RoomRowView>();
            }

            RoomRowView[] rows = new RoomRowView[rooms.Length];
            for (int index = 0; index < rooms.Length; ++index)
            {
                PlayerRoomListEntry room = rooms[index];
                bool joinEnabled = IsJoinable(room);
                string status = RoomStatusText(room);
                rows[index] = new RoomRowView(
                    room.RoomId,
                    DisplayTitle(room),
                    room.PlayerCount,
                    room.MaxPlayers,
                    room.Status,
                    status,
                    joinEnabled,
                    JoinRoomLabel);
            }

            return rows;
        }

        private static string DisplayTitle(PlayerRoomListEntry room)
        {
            return string.IsNullOrWhiteSpace(room.Title) ?
                $"#{room.RoomId}" :
                room.Title;
        }

        private static bool IsJoinable(PlayerRoomListEntry room)
        {
            return room.RoomId != 0U &&
                room.Status == PlayerRoomStatus.Open &&
                room.PlayerCount < room.MaxPlayers;
        }

        private static int ClampCreateRoomCapacity(int maxPlayers)
        {
            return Mathf.Clamp(
                maxPlayers,
                PlayerTcpPacket.CreateRoomMinCapacity,
                PlayerTcpPacket.CreateRoomMaxCapacity);
        }

        private static string RoomStatusText(PlayerRoomListEntry room)
        {
            if (room.Status == PlayerRoomStatus.InProgress)
            {
                return InProgressStatusText;
            }

            if (room.PlayerCount >= room.MaxPlayers)
            {
                return FullStatusText;
            }

            return string.Empty;
        }

        private void OnGUI()
        {
            using (PlayerManualGuiScaler.Begin(Screen.height))
            {
                GUILayout.BeginArea(new Rect(24.0f, 24.0f, 420.0f, 560.0f), GUI.skin.box);
                GUILayout.Label(LobbyLabel);
                if (!string.IsNullOrEmpty(statusText))
                {
                    GUILayout.Label(statusText);
                }

                GUILayout.Label(RoomTitleLabel);
                roomTitleInput = GUILayout.TextField(roomTitleInput, 32);
                GUILayout.Label(MaxPlayersLabel);
                string maxPlayersText = GUILayout.TextField(maxPlayersInput.ToString(), 2);
                if (int.TryParse(maxPlayersText, out int parsedMaxPlayers))
                {
                    maxPlayersInput = ClampCreateRoomCapacity(parsedMaxPlayers);
                }

                bool wasEnabled = GUI.enabled;
                GUI.enabled = wasEnabled && CreateRoomEnabled;
                if (GUILayout.Button(CreateRoomLabel))
                {
                    _ = RunCreateRoomFlowAsync();
                }

                GUI.enabled = wasEnabled;

                RoomRowView[] rows = visibleRoomRows;
                for (int index = 0; index < rows.Length; ++index)
                {
                    RoomRowView row = rows[index];
                    GUILayout.Label(
                        $"{row.DisplayTitle} {row.PlayerCount}/{row.MaxPlayers} {row.StatusText}");
                    GUI.enabled = wasEnabled && row.JoinEnabled && !roomCommandInFlight && !roomSceneLoadRequested;
                    if (GUILayout.Button(row.JoinButtonText))
                    {
                        _ = RunJoinVisibleRoomFlowAsync(index);
                    }

                    GUI.enabled = wasEnabled;
                }

                GUILayout.EndArea();
            }
        }

        public readonly struct RoomRowView
        {
            public readonly uint RoomId;
            public readonly string DisplayTitle;
            public readonly ushort PlayerCount;
            public readonly ushort MaxPlayers;
            public readonly PlayerRoomStatus Status;
            public readonly string StatusText;
            public readonly bool JoinEnabled;
            public readonly string JoinButtonText;

            public RoomRowView(
                uint roomId,
                string displayTitle,
                ushort playerCount,
                ushort maxPlayers,
                PlayerRoomStatus status,
                string statusText,
                bool joinEnabled,
                string joinButtonText)
            {
                RoomId = roomId;
                DisplayTitle = displayTitle ?? string.Empty;
                PlayerCount = playerCount;
                MaxPlayers = maxPlayers;
                Status = status;
                StatusText = statusText ?? string.Empty;
                JoinEnabled = joinEnabled;
                JoinButtonText = joinButtonText ?? string.Empty;
            }
        }
    }
}
