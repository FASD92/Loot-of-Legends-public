using System;
using System.Collections.Generic;
using System.Threading;
using LootOfLegends.Battle;
using LootOfLegends.Presentation.Common;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.FinalResult
{
    public sealed class FinalResultScreenView : MonoBehaviour, IFinalResultView
    {
        [SerializeField] private GameObject panel;
        [SerializeField] private Text headingLabel;
        [SerializeField] private Text outcomeLabel;
        [SerializeField] private Transform resultListRoot;
        [SerializeField] private GameObject resultRowPrefab;
        [SerializeField] private Text rematchLabel;
        [SerializeField] private Button roomButton;
        [SerializeField] private Button lobbyButton;
        [SerializeField] private PresentationCatalog catalog;
        [SerializeField] private AudioSource audioSource;

        private readonly List<GameObject> rows = new List<GameObject>();
        private FinalResultPresenter presenter;
        private CancellationToken cancellationToken;
        private FinalResultPresentationSnapshot snapshot;
        private bool visible;
        private ulong localSessionId;
        private ulong hostSessionId;

        public FinalResultPresentationSnapshot Snapshot => snapshot;
        public bool IsVisible => visible;

        public void SetPlayerContext(ulong localId, ulong hostId)
        {
            localSessionId = localId;
            hostSessionId = hostId;
        }

        private void Awake()
        {
            panel?.SetActive(false);
            roomButton?.onClick.AddListener(ReturnToRoom);
            lobbyButton?.onClick.AddListener(ReturnToLobby);
            SetActions(false, "경기 종료 처리를 기다리고 있습니다.");
        }

        private void OnDestroy()
        {
            roomButton?.onClick.RemoveListener(ReturnToRoom);
            lobbyButton?.onClick.RemoveListener(ReturnToLobby);
        }

        public void Bind(
            FinalResultPresenter resultPresenter,
            CancellationToken screenCancellationToken)
        {
            presenter = resultPresenter ??
                throw new ArgumentNullException(nameof(resultPresenter));
            cancellationToken = screenCancellationToken;
        }

        public void Show(FinalResultPresentationSnapshot next)
        {
            bool firstShow = !visible;
            snapshot = next ?? throw new ArgumentNullException(nameof(next));
            visible = true;
            panel?.SetActive(true);
            if (firstShow && catalog != null && audioSource != null)
            {
                audioSource.PlayOneShot(catalog.Resolve<AudioClip>("audio.sfx.result"));
            }
            RefreshView();
        }

        public void SetActions(bool enabled, string statusCopy)
        {
            if (rematchLabel != null)
            {
                rematchLabel.text = statusCopy;
            }
            if (roomButton != null)
            {
                roomButton.interactable = enabled;
            }
            if (lobbyButton != null)
            {
                lobbyButton.interactable = enabled;
            }
        }

        public void Hide()
        {
            visible = false;
            snapshot = null;
            panel?.SetActive(false);
            ClearRows();
        }

        private void RefreshView()
        {
            if (headingLabel != null)
            {
                headingLabel.text = "전투 결과";
            }
            if (outcomeLabel != null)
            {
                outcomeLabel.text = OutcomeCopy(snapshot.Outcome);
            }
            ClearRows();
            if (resultListRoot == null || resultRowPrefab == null)
            {
                return;
            }
            foreach (FinalResultPresentationRow result in snapshot.Rows)
            {
                GameObject row = Instantiate(resultRowPrefab, resultListRoot, false);
                row.name = "ResultRow-" + result.SessionId;
                row.SetActive(true);
                FinalResultRowView rowView = row.GetComponent<FinalResultRowView>();
                if (rowView == null)
                {
                    throw new InvalidOperationException(
                        "Final Result row prefab must provide FinalResultRowView");
                }
                rowView.Render(
                    result,
                    result.SessionId == localSessionId,
                    result.SessionId == hostSessionId);
                rows.Add(row);
            }
        }

        private void ReturnToRoom()
        {
            presenter?.ReturnToRoom();
        }

        private void ReturnToLobby()
        {
            _ = presenter?.ReturnToLobbyAsync(cancellationToken);
        }

        private static string OutcomeCopy(FinalResultPresentationOutcome outcome)
        {
            switch (outcome)
            {
                case FinalResultPresentationOutcome.MonsterDefeated:
                    return "몬스터 처치";
                case FinalResultPresentationOutcome.CombatTimeout:
                    return "전투 시간 초과";
                default:
                    return "전투 취소";
            }
        }

        private void ClearRows()
        {
            foreach (GameObject row in rows)
            {
                if (row != null)
                {
                    row.SetActive(false);
                    Destroy(row);
                }
            }
            rows.Clear();
        }
    }
}
