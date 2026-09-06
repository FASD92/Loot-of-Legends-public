using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.Common;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Room
{
    public sealed class RoomScreenView : MonoBehaviour, IRoomView
    {
        private const string WaitingForProjectionCopy =
            "서버 방 정보를 기다리는 중입니다.";

        private static readonly Color ReadyMemberColor =
            new Color32(0x3F, 0x63, 0x2C, 0xFF);
        private static readonly Color NotReadyMemberColor =
            new Color32(0x5A, 0x35, 0x1B, 0xFF);
        private static readonly Color LocalRowColor =
            new Color32(0xFF, 0xF2, 0xD4, 0xFF);

        // 강퇴 버튼은 런타임에 만들어 멤버 행 안에 넣으므로 프리팹 에셋 테스트
        // (Selectable 높이 >= 54, fontSize >= 24)가 검사하지 않는다. 그 결과 80x28 / 18pt 로
        // 남아 거의 안 읽혔다. ListRow 높이가 48 이라 표준 54 는 들어가지 않고, 40 이 위아래
        // 4 여백을 남기는 최대치다. 글자 크기는 저장소 최소값 24 로 올렸다.
        private static readonly Vector2 KickButtonSize = new Vector2(112f, 40f);
        private const int KickButtonFontSize = 24;

        // Kenney 버튼 스프라이트는 5px border 이고 PPU 32, Canvas 100 이라 기본 배율에서
        // border 가 15.6 canvas unit 이 된다. 높이 40 에서는 위아래가 31 을 먹어 프레임만 남는다.
        // 100/32 로 올리면 border 가 원본과 같은 5 unit 이 되어 내부 30 이 남는다.
        // 배율을 키우면 border 가 줄어든다 — 직관과 반대다.
        private const float KickButtonPixelsPerUnitMultiplier = 3.125f;

        [SerializeField] private GameObject panel;
        [SerializeField] private Text titleLabel;
        [SerializeField] private Text statusLabel;
        [SerializeField] private Transform memberListRoot;
        [SerializeField] private GameObject memberRowPrefab;
        [SerializeField] private Button readyButton;
        [SerializeField] private Button startButton;
        [SerializeField] private Button leaveButton;

        private readonly List<GameObject> memberRows = new List<GameObject>();
        private readonly List<Button> kickButtons = new List<Button>();
        private RoomPresenter presenter;
        private ActionDialogView kickConfirmation;
        private CancellationToken cancellationToken;
        private RoomPresentationSnapshot snapshot;
        private Task pending;
        private bool awaitingLobbyProjection;
        private string statusCopy = WaitingForProjectionCopy;

        public RoomPresentationSnapshot Snapshot => snapshot;
        public string StatusCopy => snapshot != null &&
            statusCopy == WaitingForProjectionCopy
                ? snapshot.Members.Count >= 2 &&
                    snapshot.Members.All(member => member.Ready)
                    ? "모든 플레이어가 준비되었습니다."
                    : "플레이어의 준비를 기다리고 있습니다."
                : statusCopy;

        private void Awake()
        {
            if (panel != null)
            {
                panel.SetActive(true);
            }
            readyButton?.onClick.AddListener(BeginReadyToggle);
            startButton?.onClick.AddListener(BeginHostStart);
            leaveButton?.onClick.AddListener(BeginLeave);
            kickConfirmation = ActionDialogView.Create(
                panel?.transform,
                panel?.GetComponent<Image>(),
                statusLabel,
                readyButton);
            RefreshView();
        }

        private void OnDestroy()
        {
            readyButton?.onClick.RemoveListener(BeginReadyToggle);
            startButton?.onClick.RemoveListener(BeginHostStart);
            leaveButton?.onClick.RemoveListener(BeginLeave);
        }

        public void Bind(
            RoomPresenter screenPresenter,
            CancellationToken screenCancellationToken)
        {
            presenter = screenPresenter ??
                throw new ArgumentNullException(nameof(screenPresenter));
            cancellationToken = screenCancellationToken;
            RefreshControls();
        }

        public void Render(RoomPresentationSnapshot next)
        {
            snapshot = next;
            RefreshView();
        }

        public void ShowCommandResult(RoomCommandResult result)
        {
            if (awaitingLobbyProjection && result != RoomCommandResult.Ok)
            {
                awaitingLobbyProjection = false;
            }
            statusCopy = result == RoomCommandResult.Ok
                ? "요청이 접수되었습니다. 서버 상태 반영을 기다립니다."
                : "요청이 거절되었습니다: " + result;
            RefreshStatus();
        }

        public Task SetReadyAsync(bool ready)
        {
            EnsureCanBegin();
            statusCopy = "준비 상태 변경 요청 중입니다.";
            RefreshStatus();
            pending = presenter.SetReadyAsync(ready, cancellationToken);
            RefreshControls();
            return pending;
        }

        public Task HostStartAsync()
        {
            EnsureCanBegin();
            statusCopy = "게임 시작 요청 중입니다.";
            RefreshStatus();
            pending = presenter.HostStartAsync(cancellationToken);
            RefreshControls();
            return pending;
        }

        public Task LeaveAsync()
        {
            EnsureCanBegin();
            awaitingLobbyProjection = true;
            statusCopy = "방 나가기 요청 중입니다.";
            RefreshStatus();
            pending = presenter.LeaveAsync(cancellationToken);
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
                awaitingLobbyProjection = false;
                statusCopy = "요청을 완료하지 못했습니다.";
                RefreshStatus();
            }
            pending = null;
            RefreshControls();
        }

        private void BeginReadyToggle()
        {
            RoomMemberPresentation local = snapshot?.Members.FirstOrDefault(
                member => member.IsLocal);
            if (local != null)
            {
                _ = SetReadyAsync(!local.Ready);
            }
        }

        private void BeginHostStart()
        {
            _ = HostStartAsync();
        }

        private void BeginLeave()
        {
            _ = LeaveAsync();
        }

        private void BeginKickConfirmation(RoomMemberPresentation member)
        {
            if (presenter == null || pending != null || awaitingLobbyProjection ||
                snapshot?.IsLocalHost != true || member.IsLocal)
            {
                return;
            }
            kickConfirmation.ShowConfirmation(
                member.Nickname + " 님을 강퇴할까요?",
                "강퇴",
                () => BeginKick(member.SessionId, member.SessionGeneration),
                "취소",
                RefreshControls);
            RefreshControls();
        }

        private void BeginKick(ulong sessionId, ulong sessionGeneration)
        {
            EnsureCanBegin();
            statusCopy = "강퇴 요청 중입니다.";
            RefreshStatus();
            pending = presenter.KickAsync(
                sessionId,
                sessionGeneration,
                cancellationToken);
            RefreshControls();
        }

        private void RefreshView()
        {
            if (titleLabel != null)
            {
                titleLabel.text = snapshot == null
                    ? WaitingForProjectionCopy
                    : $"{snapshot.Title}\n{snapshot.Members.Count} / {snapshot.Capacity}";
            }
            RefreshStatus();
            RebuildMemberList();
            RefreshControls();
        }

        private void RefreshStatus()
        {
            if (statusLabel != null)
            {
                statusLabel.text = StatusCopy;
            }
        }

        private void RebuildMemberList()
        {
            foreach (GameObject row in memberRows)
            {
                if (row != null)
                {
                    Destroy(row);
                }
            }
            memberRows.Clear();
            kickButtons.Clear();
            if (snapshot == null || memberListRoot == null || memberRowPrefab == null)
            {
                return;
            }

            foreach (RoomMemberPresentation member in snapshot.Members)
            {
                GameObject row = Instantiate(memberRowPrefab, memberListRoot, false);
                row.name = "MemberRow-" + member.SessionId;
                row.SetActive(true);
                Text label = row.GetComponentInChildren<Text>(true);
                if (label != null)
                {
                    string role = member.IsHost
                        ? member.IsLocal ? "방장 · 나" : "방장"
                        : member.IsLocal ? "나" : string.Empty;
                    string ready = member.Ready ? "준비 완료" : "준비 전";
                    label.text = string.IsNullOrEmpty(role)
                        ? $"{member.Nickname}   {ready}"
                        : $"{role}   {member.Nickname}   {ready}";
                    label.color = member.Ready
                        ? ReadyMemberColor
                        : NotReadyMemberColor;
                }
                Image rowImage = row.GetComponent<Image>();
                if (rowImage != null)
                {
                    rowImage.color = member.IsLocal ? LocalRowColor : Color.white;
                }
                if (snapshot.IsLocalHost && !member.IsLocal &&
                    kickConfirmation != null && leaveButton != null)
                {
                    Button kick = Instantiate(leaveButton, row.transform, false);
                    kick.name = "KickButton-" + member.SessionId;
                    kick.onClick = new Button.ButtonClickedEvent();
                    RectTransform kickRect = kick.GetComponent<RectTransform>();
                    kickRect.anchorMin = kickRect.anchorMax = new Vector2(1f, 0.5f);
                    kickRect.pivot = new Vector2(1f, 0.5f);
                    kickRect.anchoredPosition = new Vector2(-8f, 0f);
                    kickRect.sizeDelta = KickButtonSize;
                    if (kick.image != null)
                    {
                        kick.image.pixelsPerUnitMultiplier =
                            KickButtonPixelsPerUnitMultiplier;
                    }
                    RoomMemberPresentation target = member;
                    kick.onClick.AddListener(() => BeginKickConfirmation(target));
                    ActionDialogView.SetButtonLabel(kick, "강퇴", KickButtonFontSize);
                    kick.gameObject.SetActive(true);
                    kickButtons.Add(kick);
                }
                memberRows.Add(row);
            }
        }

        private void RefreshControls()
        {
            bool idle = presenter != null && pending == null &&
                !awaitingLobbyProjection && kickConfirmation?.IsVisible != true;
            RoomMemberPresentation local = snapshot?.Members.FirstOrDefault(
                member => member.IsLocal);
            if (readyButton != null)
            {
                readyButton.interactable = idle && snapshot != null &&
                    snapshot.CanToggleReady && local != null;
                Text label = readyButton.GetComponentInChildren<Text>(true);
                if (label != null)
                {
                    label.text = local != null && local.Ready ? "준비 취소" : "준비";
                }
            }
            if (startButton != null)
            {
                startButton.interactable = idle && snapshot != null && snapshot.CanStart;
            }
            if (leaveButton != null)
            {
                leaveButton.interactable = idle && snapshot != null;
            }
            foreach (Button kick in kickButtons)
            {
                kick.interactable = idle && snapshot?.IsLocalHost == true;
            }
        }

        private void EnsureCanBegin()
        {
            if (presenter == null)
            {
                throw new InvalidOperationException("Room screen is not bound");
            }
            if (pending != null || awaitingLobbyProjection)
            {
                throw new InvalidOperationException("Room request is already pending");
            }
        }
    }
}
