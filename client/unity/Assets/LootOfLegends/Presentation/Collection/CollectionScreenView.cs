using System;
using System.Collections.Generic;
using LootOfLegends.Collection;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Collection
{
    public sealed class CollectionScreenView : MonoBehaviour, ICollectionView
    {
        [SerializeField] private GameObject panel;
        [SerializeField] private CanvasGroup panelGroup;
        [SerializeField] private Button toggleButton;
        [SerializeField] private Text walletLabel;
        [SerializeField] private Text pendingLabel;
        [SerializeField] private Text statusLabel;
        [SerializeField] private Text emptyLabel;
        [SerializeField] private Transform itemListRoot;
        [SerializeField] private GameObject itemRowPrefab;
        [SerializeField] private Button refreshButton;

        private readonly List<GameObject> rows = new List<GameObject>();
        private Action requestRefresh;
        private CollectionPresentationSnapshot snapshot;

        private static readonly Color StatusColor =
            new Color32(90, 53, 27, 255);
        private static readonly Color StaleStatusColor =
            new Color32(166, 103, 47, 255);

        public CollectionPresentationSnapshot Snapshot => snapshot;

        private void Awake()
        {
            panel?.SetActive(true);
            toggleButton?.onClick.AddListener(TogglePanel);
            refreshButton?.onClick.AddListener(RequestRefresh);
            SetPanelVisible(false);
            RefreshView();
        }

        private void OnDestroy()
        {
            toggleButton?.onClick.RemoveListener(TogglePanel);
            refreshButton?.onClick.RemoveListener(RequestRefresh);
        }

        public void Bind(Action screenRefresh)
        {
            requestRefresh = screenRefresh ??
                throw new ArgumentNullException(nameof(screenRefresh));
            RefreshControls();
        }

        public void Render(CollectionPresentationSnapshot next)
        {
            snapshot = next ?? throw new ArgumentNullException(nameof(next));
            RefreshView();
        }

        public void RequestRefresh()
        {
            if (requestRefresh == null)
            {
                throw new InvalidOperationException("Collection screen is not bound");
            }
            requestRefresh();
        }

        private void TogglePanel()
        {
            SetPanelVisible(panelGroup == null || panelGroup.alpha < 0.5f);
        }

        private void SetPanelVisible(bool visible)
        {
            if (panelGroup != null)
            {
                panelGroup.alpha = visible ? 1f : 0f;
                panelGroup.interactable = visible;
                panelGroup.blocksRaycasts = visible;
            }
            Text label = toggleButton == null
                ? null
                : toggleButton.GetComponentInChildren<Text>(true);
            if (label != null)
            {
                label.text = visible ? "닫기" : "컬렉션";
            }
        }

        private void RefreshView()
        {
            if (walletLabel != null)
            {
                walletLabel.text = "보유 재화  " + (snapshot?.Wallet ?? 0);
            }
            if (pendingLabel != null)
            {
                pendingLabel.text = "정산 대기  " +
                    (snapshot?.PendingSettlementCount ?? 0) + "건";
            }
            if (statusLabel != null)
            {
                string statusCopy = snapshot?.StatusCopy ??
                    "컬렉션을 불러오는 중입니다.";
                statusLabel.text = statusCopy;
                statusLabel.gameObject.SetActive(!string.IsNullOrEmpty(statusCopy));
                statusLabel.color = snapshot?.State == CollectionPresentationState.Stale
                    ? StaleStatusColor
                    : StatusColor;
            }
            if (emptyLabel != null)
            {
                bool showEmpty = snapshot?.State == CollectionPresentationState.Fresh &&
                    snapshot.Items.Count == 0;
                emptyLabel.text = "보유한 아이템이 없습니다.";
                emptyLabel.gameObject.SetActive(showEmpty);
            }

            ClearRows();
            if (snapshot != null && itemListRoot != null && itemRowPrefab != null)
            {
                foreach (CollectionPresentationItem item in snapshot.Items)
                {
                    GameObject row = Instantiate(itemRowPrefab, itemListRoot, false);
                    row.name = "CollectionItem-" + item.ItemId;
                    row.SetActive(true);
                    CollectionItemRowView rowView =
                        row.GetComponent<CollectionItemRowView>();
                    if (rowView == null)
                    {
                        throw new InvalidOperationException(
                            "Collection item row prefab must provide CollectionItemRowView");
                    }
                    rowView.Render(item);
                    rows.Add(row);
                }
            }
            RefreshControls();
        }

        private void RefreshControls()
        {
            if (refreshButton != null)
            {
                refreshButton.interactable = requestRefresh != null && snapshot != null &&
                    snapshot?.State != CollectionPresentationState.Loading;
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
