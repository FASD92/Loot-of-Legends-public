using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.Common;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Lobby
{
    public sealed class LobbyScreenView : MonoBehaviour, ILobbyView
    {
        private const string WaitingForProjectionCopy =
            "서버 로비 정보를 기다리는 중입니다.";
        private const string LoadedGuidanceCopy =
            "입장할 방을 선택하거나 새 방을 만드세요.";
        private const byte MinimumCapacity = 2;
        private const byte MaximumCapacity = 10;

        [SerializeField] private GameObject panel;
        [SerializeField] private Text nicknameLabel;
        [SerializeField] private Text statusLabel;
        [SerializeField] private InputField roomTitleInput;
        [SerializeField] private InputField capacityInput;
        [SerializeField] private Button createButton;
        [SerializeField] private Transform roomListRoot;
        [SerializeField] private Button roomButtonPrefab;

        private readonly Dictionary<Button, bool> roomButtons =
            new Dictionary<Button, bool>();
        private LobbyPresenter presenter;
        private ActionDialogView kickNotice;
        private CancellationToken cancellationToken;
        private LobbyPresentationSnapshot snapshot;
        private Task pending;
        private string statusCopy = WaitingForProjectionCopy;

        public LobbyPresentationSnapshot Snapshot => snapshot;
        public string StatusCopy => statusCopy;

        private void Awake()
        {
            if (panel != null)
            {
                panel.SetActive(true);
            }
            if (createButton != null)
            {
                createButton.onClick.AddListener(BeginCreate);
            }
            kickNotice = ActionDialogView.Create(
                panel?.transform,
                panel?.GetComponent<Image>(),
                statusLabel,
                createButton);
            RefreshView();
        }

        private void OnDestroy()
        {
            if (createButton != null)
            {
                createButton.onClick.RemoveListener(BeginCreate);
            }
        }

        public void Bind(
            LobbyPresenter screenPresenter,
            CancellationToken screenCancellationToken)
        {
            presenter = screenPresenter ??
                throw new ArgumentNullException(nameof(screenPresenter));
            cancellationToken = screenCancellationToken;
            RefreshControls();
        }

        public void Render(LobbyPresentationSnapshot next)
        {
            snapshot = next ?? throw new ArgumentNullException(nameof(next));
            if (snapshot.Revision != 0 && statusCopy == WaitingForProjectionCopy)
            {
                statusCopy = LoadedGuidanceCopy;
            }
            RefreshView();
        }

        public void ShowCommandResult(RoomCommandResult result)
        {
            statusCopy = result == RoomCommandResult.Ok
                ? "요청이 접수되었습니다. 서버 상태 반영을 기다립니다."
                : "요청이 거절되었습니다: " + result;
            RefreshStatus();
        }

        public void ShowKickedNotice()
        {
            kickNotice?.ShowNotice("방장에 의해 강퇴되었습니다.");
        }

        public Task CreateAsync(string title)
        {
            EnsureCanBegin();
            if (!TryReadCapacity(out byte capacity))
            {
                statusCopy = "인원은 2~10명으로 입력해 주세요.";
                RefreshStatus();
                return Task.CompletedTask;
            }
            statusCopy = "방 생성 요청 중입니다.";
            RefreshStatus();
            try
            {
                pending = presenter.CreateAsync(title, capacity, cancellationToken);
            }
            catch (ArgumentException)
            {
                statusCopy = "방 제목을 확인해 주세요.";
                RefreshStatus();
                return Task.CompletedTask;
            }
            RefreshControls();
            return pending;
        }

        public Task JoinAsync(ulong roomId)
        {
            EnsureCanBegin();
            statusCopy = "방 참가 요청 중입니다.";
            RefreshStatus();
            pending = presenter.JoinAsync(roomId, cancellationToken);
            RefreshControls();
            return pending;
        }

        private void Update()
        {
            if (pending == null || !pending.IsCompleted)
            {
                return;
            }
            if (pending.IsFaulted || pending.IsCanceled)
            {
                statusCopy = "요청을 완료하지 못했습니다.";
                RefreshStatus();
            }
            pending = null;
            RefreshControls();
        }

        private void BeginCreate()
        {
            _ = CreateAsync(roomTitleInput == null
                ? "Player Room"
                : roomTitleInput.text);
        }

        private void RefreshView()
        {
            if (nicknameLabel != null)
            {
                nicknameLabel.text = snapshot == null || snapshot.Revision == 0
                    ? WaitingForProjectionCopy
                    : snapshot.Nickname;
            }
            RefreshStatus();
            RebuildRoomList();
            RefreshControls();
        }

        private void RefreshStatus()
        {
            if (statusLabel != null)
            {
                statusLabel.text = statusCopy;
            }
        }

        private void RebuildRoomList()
        {
            foreach (Button button in roomButtons.Keys)
            {
                if (button != null)
                {
                    Destroy(button.gameObject);
                }
            }
            roomButtons.Clear();
            if (snapshot == null || roomListRoot == null || roomButtonPrefab == null)
            {
                return;
            }

            foreach (LobbyRoomSummaryView room in snapshot.Rooms)
            {
                Button button = Instantiate(roomButtonPrefab, roomListRoot, false);
                button.name = "RoomButton-" + room.RoomId;
                button.gameObject.SetActive(true);
                Text label = button.GetComponentInChildren<Text>(true);
                if (label != null)
                {
                    label.text = $"{room.Title}   {room.MemberCount}/{room.Capacity}" +
                        (room.IsFull ? "   만석" : string.Empty);
                    label.color = room.IsFull
                        ? new Color32(0x5A, 0x35, 0x1B, 0xFF)
                        : Color.white;
                }
                ulong roomId = room.RoomId;
                button.onClick.AddListener(() => _ = JoinAsync(roomId));
                roomButtons.Add(button, room.IsFull);
            }
        }

        private void RefreshControls()
        {
            bool idle = presenter != null && pending == null;
            if (createButton != null)
            {
                createButton.interactable = idle;
            }
            if (roomTitleInput != null)
            {
                roomTitleInput.interactable = idle;
            }
            if (capacityInput != null)
            {
                capacityInput.interactable = idle;
            }
            foreach (KeyValuePair<Button, bool> room in roomButtons)
            {
                if (room.Key != null)
                {
                    room.Key.interactable = idle && !room.Value;
                }
            }
        }

        private bool TryReadCapacity(out byte capacity)
        {
            string value = capacityInput == null ? "2" : capacityInput.text;
            return byte.TryParse(value, out capacity) &&
                capacity >= MinimumCapacity && capacity <= MaximumCapacity;
        }

        private void EnsureCanBegin()
        {
            if (presenter == null)
            {
                throw new InvalidOperationException("Lobby screen is not bound");
            }
            if (pending != null)
            {
                throw new InvalidOperationException("Lobby request is already pending");
            }
        }
    }
}
