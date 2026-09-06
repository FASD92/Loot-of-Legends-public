using System;
using System.Collections;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Collection;
using LootOfLegends.Presentation.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.Lobby;
using LootOfLegends.Transport;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using UnityEngine.UI;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class CollectionPresentationTests
    {
        [UnityTest]
        public IEnumerator LobbyCollectionOpensOnlyFromItsToggle()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            CollectionScreenView view = UnityEngine.Object
                .FindFirstObjectByType<CollectionScreenView>();
            LobbyScreenView lobby = UnityEngine.Object
                .FindFirstObjectByType<LobbyScreenView>();
            Button toggle = FindButton(lobby.transform, "CollectionToggleButton");
            CanvasGroup group = view.GetComponent<CanvasGroup>();
            Text toggleLabel = toggle.GetComponentInChildren<Text>(true);

            Assert.That(group.alpha, Is.Zero);
            Assert.That(group.interactable, Is.False);
            Assert.That(group.blocksRaycasts, Is.False);
            Assert.That(toggleLabel.text, Is.EqualTo("컬렉션"));

            toggle.onClick.Invoke();
            yield return null;
            Assert.That(group.alpha, Is.EqualTo(1f));
            Assert.That(group.interactable, Is.True);
            Assert.That(group.blocksRaycasts, Is.True);
            Assert.That(toggleLabel.text, Is.EqualTo("닫기"));

            toggle.onClick.Invoke();
            yield return null;
            Assert.That(group.alpha, Is.Zero);
            Assert.That(group.interactable, Is.False);
            Assert.That(group.blocksRaycasts, Is.False);
            Assert.That(toggleLabel.text, Is.EqualTo("컬렉션"));
        }

        [UnityTest]
        public IEnumerator RefreshShowsOnlyAppliedQuantityAndSeparatePendingCount()
        {
            var model = new CollectionReadModel();
            model.Apply(Snapshot(quantity: 4, wallet: 300, pending: 1));
            var source = new RecordingCollectionApi(
                Snapshot(quantity: 4, wallet: 300, pending: 2));
            var view = new RecordingCollectionView();
            var presenter = new CollectionPresenter(model, source, view);

            Task refresh = presenter.RefreshAsync(CancellationToken.None);
            yield return new WaitUntil(() => refresh.IsCompleted);

            Assert.That(source.Calls, Is.EqualTo(1));
            Assert.That(view.States, Is.EqualTo(new[]
            {
                CollectionPresentationState.Loading,
                CollectionPresentationState.Fresh
            }));
            Assert.That(view.Last.Items, Has.Count.EqualTo(1));
            Assert.That(view.Last.Items[0].Quantity, Is.EqualTo(4),
                "pending settlement must not predict Applied quantity");
            Assert.That(view.Last.Wallet, Is.EqualTo(300));
            Assert.That(view.Last.PendingSettlementCount, Is.EqualTo(2));
        }

        [UnityTest]
        public IEnumerator FailedRefreshKeepsConfirmedDataAsStaleWithBoundedCopy()
        {
            var model = new CollectionReadModel();
            model.Apply(Snapshot(quantity: 4, wallet: 300, pending: 1));
            var view = new RecordingCollectionView();
            var presenter = new CollectionPresenter(
                model,
                new ThrowingCollectionApi(
                    "Bearer secret-token https://10.0.0.8/internal stack trace"),
                view);

            Task refresh = presenter.RefreshAsync(CancellationToken.None);
            yield return new WaitUntil(() => refresh.IsCompleted);

            Assert.That(refresh.IsFaulted, Is.False);
            Assert.That(view.Last.State, Is.EqualTo(CollectionPresentationState.Stale));
            Assert.That(view.Last.Items[0].Quantity, Is.EqualTo(4));
            Assert.That(view.Last.StatusCopy, Does.Not.Contain("Bearer"));
            Assert.That(view.Last.StatusCopy, Does.Not.Contain("10.0.0.8"));
            Assert.That(view.Last.StatusCopy, Does.Not.Contain("stack"));
            Assert.That(view.Last.StatusCopy.Length, Is.LessThanOrEqualTo(80));
        }

        [UnityTest]
        public IEnumerator InitialFailureShowsErrorWithoutInventingCollection()
        {
            var model = new CollectionReadModel();
            var owner = new GameObject("CollectionScreenViewTest");
            var view = owner.AddComponent<CollectionScreenView>();
            try
            {
                var presenter = new CollectionPresenter(
                    model,
                    new ThrowingCollectionApi("raw-internal-error"),
                    view);
                Task refresh = null;
                view.Bind(() =>
                    refresh = presenter.RefreshAsync(CancellationToken.None));

                view.RequestRefresh();
                yield return new WaitUntil(() => refresh.IsCompleted);

                Assert.That(view.Snapshot.State,
                    Is.EqualTo(CollectionPresentationState.Error));
                Assert.That(view.Snapshot.Items, Is.Empty);
                Assert.That(view.Snapshot.Wallet, Is.Zero);
                Assert.That(view.Snapshot.PendingSettlementCount, Is.Zero);
                Assert.That(view.Snapshot.StatusCopy,
                    Does.Not.Contain("raw-internal-error"));
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(owner);
            }
        }

        [UnityTest]
        public IEnumerator RefreshIgnoresAViewDestroyedBySceneTransition()
        {
            var model = new CollectionReadModel();
            var source = new DeferredCollectionApi();
            var root = new GameObject("CollectionView");
            var view = root.AddComponent<CollectionScreenView>();
            var presenter = new CollectionPresenter(model, source, view);

            Task refresh = presenter.RefreshAsync(CancellationToken.None);
            UnityEngine.Object.Destroy(root);
            yield return null;
            source.Complete(Snapshot(quantity: 1, wallet: 100, pending: 0));
            yield return new WaitUntil(() => refresh.IsCompleted);

            Assert.That(refresh.IsFaulted, Is.False);
        }

        [UnityTest]
        public IEnumerator LobbyCollectionRendersConfirmedDataAsColumnsAcrossStates()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            var view = UnityEngine.Object.FindFirstObjectByType<CollectionScreenView>(
                FindObjectsInactive.Include);
            Assert.That(view, Is.Not.Null);

            Transform listRoot = FindTransform(view.transform, "CollectionItemList");
            Text walletLabel = FindText(view.transform, "CollectionWalletLabel");
            Text pendingLabel = FindText(view.transform, "CollectionPendingLabel");
            Text statusLabel = FindText(view.transform, "CollectionStatusLabel");
            Text emptyLabel = FindText(view.transform, "CollectionEmptyLabel");
            Button refreshButton = FindButton(view.transform, "CollectionRefreshButton");

            Assert.That(listRoot, Is.Not.Null);
            Assert.That(walletLabel, Is.Not.Null);
            Assert.That(pendingLabel, Is.Not.Null);
            Assert.That(statusLabel, Is.Not.Null);
            Assert.That(emptyLabel, Is.Not.Null);
            Assert.That(refreshButton, Is.Not.Null);
            Assert.That(view.GetComponentsInChildren<Button>(true), Has.Length.EqualTo(1));

            int refreshCalls = 0;
            view.Bind(() => refreshCalls++);

            var items = new[]
            {
                new CollectionPresentationItem(1, 4, 100),
                new CollectionPresentationItem(2, 1, 300)
            };
            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Fresh, items, 1200, 2));
            yield return null;

            Assert.That(walletLabel.text, Is.EqualTo("보유 재화  1200"));
            Assert.That(pendingLabel.text, Is.EqualTo("정산 대기  2건"));
            Assert.That(statusLabel.gameObject.activeSelf, Is.False);
            Assert.That(emptyLabel.gameObject.activeSelf, Is.False);
            Assert.That(refreshButton.interactable, Is.True);
            AssertColumnRows(listRoot, new[] { "1", "2" },
                new[] { "4", "1" }, new[] { "100", "300" });

            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Stale, items, 1200, 2));
            yield return null;

            Assert.That(statusLabel.gameObject.activeSelf, Is.True);
            Assert.That(statusLabel.text,
                Is.EqualTo("최근 확인된 컬렉션입니다. 다시 시도해 주세요."));
            Assert.That(walletLabel.text, Is.EqualTo("보유 재화  1200"));
            Assert.That(pendingLabel.text, Is.EqualTo("정산 대기  2건"));
            Assert.That(refreshButton.interactable, Is.True);
            AssertColumnRows(listRoot, new[] { "1", "2" },
                new[] { "4", "1" }, new[] { "100", "300" });

            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Loading, items, 1200, 2));
            yield return null;

            Assert.That(statusLabel.text,
                Is.EqualTo("컬렉션을 불러오는 중입니다."));
            Assert.That(refreshButton.interactable, Is.False);
            AssertColumnRows(listRoot, new[] { "1", "2" },
                new[] { "4", "1" }, new[] { "100", "300" });

            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Error,
                new CollectionPresentationItem[0], 0, 0));
            yield return null;

            Assert.That(statusLabel.text,
                Is.EqualTo("컬렉션을 불러오지 못했습니다. 다시 시도해 주세요."));
            Assert.That(ActiveItemRows(listRoot), Is.Empty);
            Assert.That(emptyLabel.gameObject.activeSelf, Is.False);
            Assert.That(refreshButton.interactable, Is.True);

            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Fresh,
                new CollectionPresentationItem[0], 0, 0));
            yield return null;

            Assert.That(statusLabel.gameObject.activeSelf, Is.False);
            Assert.That(emptyLabel.gameObject.activeSelf, Is.True);
            Assert.That(emptyLabel.text, Is.EqualTo("보유한 아이템이 없습니다."));
            Assert.That(refreshButton.interactable, Is.True);

            refreshButton.onClick.Invoke();
            Assert.That(refreshCalls, Is.EqualTo(1));
        }

        [UnityTest]
        public IEnumerator KnownRelicsUseCatalogIconsAndUnknownItemsKeepNumericFallback()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            CollectionScreenView view = UnityEngine.Object
                .FindFirstObjectByType<CollectionScreenView>(FindObjectsInactive.Include);
            Transform listRoot = FindTransform(view.transform, "CollectionItemList");
            view.Bind(() => { });
            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Fresh,
                new[]
                {
                    new CollectionPresentationItem(1, 4, 100),
                    new CollectionPresentationItem(2, 1, 300),
                    new CollectionPresentationItem(20, 2, 900)
                }, 1200, 2));
            yield return null;

            List<Transform> rows = ActiveItemRows(listRoot);
            Image commonIcon = FindImage(rows[0], "ItemIcon");
            Image rareIcon = FindImage(rows[1], "ItemIcon");
            Image unknownIcon = FindImage(rows[2], "ItemIcon");
            Assert.That(commonIcon, Is.Not.Null);
            Assert.That(rareIcon, Is.Not.Null);
            Assert.That(unknownIcon, Is.Not.Null);
            Assert.That(commonIcon.gameObject.activeSelf, Is.True);
            Assert.That(rareIcon.gameObject.activeSelf, Is.True);
            Assert.That(commonIcon.sprite, Is.Not.Null);
            Assert.That(rareIcon.sprite, Is.Not.Null);
            Assert.That(FindText(rows[0], "ItemIdLabel").gameObject.activeSelf, Is.False);
            Assert.That(FindText(rows[1], "ItemIdLabel").gameObject.activeSelf, Is.False);
            Assert.That(unknownIcon.gameObject.activeSelf, Is.False);
            Assert.That(FindText(rows[2], "ItemIdLabel").gameObject.activeSelf, Is.True);
            Assert.That(FindText(rows[2], "ItemIdLabel").text, Is.EqualTo("20"));
            Assert.That(FindText(rows[0], "QuantityLabel").text, Is.EqualTo("4"));
            Assert.That(FindText(rows[0], "ValueLabel").text, Is.EqualTo("100"));
        }

        [UnityTest]
        public IEnumerator LobbyCollectionClipsTwentyRowsInsideItsDedicatedViewport()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            var view = UnityEngine.Object.FindFirstObjectByType<CollectionScreenView>(
                FindObjectsInactive.Include);
            Assert.That(view, Is.Not.Null);

            Transform section = FindTransform(view.transform, "CollectionSection");
            Transform viewport = FindTransform(view.transform, "CollectionItemViewport");
            Transform listRoot = FindTransform(view.transform, "CollectionItemList");
            Text statusLabel = FindText(view.transform, "CollectionStatusLabel");
            Button refreshButton = FindButton(view.transform, "CollectionRefreshButton");
            Assert.That(section, Is.Not.Null);
            Assert.That(viewport, Is.Not.Null);
            Assert.That(listRoot, Is.Not.Null);
            Assert.That(viewport.GetComponent<RectMask2D>(), Is.Not.Null);

            ScrollRect scroll = listRoot.GetComponentInParent<ScrollRect>();
            Assert.That(scroll, Is.Not.Null);
            Assert.That(scroll.content, Is.EqualTo(listRoot as RectTransform));
            Assert.That(scroll.viewport, Is.EqualTo(viewport as RectTransform));
            Assert.That(scroll.vertical, Is.True);
            Assert.That(scroll.horizontal, Is.False);
            Assert.That(scroll.horizontalScrollbar, Is.Null);

            view.Bind(() => { });
            var items = new List<CollectionPresentationItem>();
            for (ulong itemId = 1; itemId <= 20; itemId++)
            {
                items.Add(new CollectionPresentationItem(itemId, itemId, itemId * 100));
            }
            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Fresh, items, 1200, 2));
            Canvas.ForceUpdateCanvases();
            yield return null;

            List<Transform> rows = ActiveItemRows(listRoot);
            Assert.That(rows, Has.Count.EqualTo(20));
            Assert.That((listRoot as RectTransform).rect.height,
                Is.GreaterThan((viewport as RectTransform).rect.height));
            Assert.That((rows[0] as RectTransform).rect.width,
                Is.EqualTo((viewport as RectTransform).rect.width).Within(0.5f));
            AssertRectInside(viewport as RectTransform, section as RectTransform);
            AssertRectInside(refreshButton.transform as RectTransform, section as RectTransform);
            AssertRectInside(rows[0] as RectTransform, viewport as RectTransform);
            Assert.That(statusLabel.gameObject.activeSelf, Is.False);

            scroll.verticalNormalizedPosition = 0f;
            Canvas.ForceUpdateCanvases();
            yield return null;
            Assert.That(FindText(rows[19], "ItemIdLabel").text, Is.EqualTo("20"));

            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Stale,
                new[] { items[0], items[1] }, 1200, 2));
            Canvas.ForceUpdateCanvases();
            yield return null;

            Assert.That(ActiveItemRows(listRoot), Has.Count.EqualTo(2));
            Assert.That(statusLabel.gameObject.activeSelf, Is.True);
            AssertRectInside(statusLabel.transform as RectTransform, section as RectTransform);
        }

        [UnityTest]
        public IEnumerator LobbyCollectionDesktopFixturesStayInsideThePanel()
        {
            foreach (Vector2Int resolution in new[]
                     {
                         new Vector2Int(1920, 1080),
                         new Vector2Int(1280, 720)
                     })
            {
                Screen.SetResolution(resolution.x, resolution.y, false);
                SceneManager.LoadScene("LobbyScene");
                yield return null;

                LobbyScreenView lobby = UnityEngine.Object
                    .FindFirstObjectByType<LobbyScreenView>();
                CollectionScreenView view = UnityEngine.Object
                    .FindFirstObjectByType<CollectionScreenView>(
                        FindObjectsInactive.Include);
                Assert.That(lobby, Is.Not.Null);
                Assert.That(view, Is.Not.Null);

                var presenter = new LobbyPresenter(
                    new LobbyRoomReadModel(),
                    new LobbyRoomCommandCoordinator(
                        new FixtureTcpSender(), new RoomCommandCorrelator()),
                    lobby);
                lobby.Bind(presenter, CancellationToken.None);
                lobby.Render(new LobbyPresentationSnapshot(
                    1,
                    "portfolio-player",
                    new[]
                    {
                        new LobbyRoomSummaryView(7, "Treasure Hunters", 1, 2),
                        new LobbyRoomSummaryView(8, "Full Expedition", 2, 2)
                    }));
                Assert.That(lobby.StatusCopy,
                    Is.EqualTo("입장할 방을 선택하거나 새 방을 만드세요."));

                InputField roomTitle = lobby.GetComponentInChildren<InputField>(true);
                Button create = FindButton(lobby.transform, "CreateButton");
                Button available = FindButton(lobby.transform, "RoomButton-7");
                Button full = FindButton(lobby.transform, "RoomButton-8");
                Assert.That(roomTitle.interactable, Is.True);
                Assert.That(create.interactable, Is.True);
                Assert.That(available.interactable, Is.True);
                Assert.That(full.interactable, Is.False);

                view.Bind(() => { });
                Transform section = view.transform;
                RectTransform panel = section.parent as RectTransform;
                RectTransform viewport = FindTransform(view.transform,
                    "CollectionItemViewport") as RectTransform;
                Text wallet = FindText(view.transform, "CollectionWalletLabel");
                Text pending = FindText(view.transform, "CollectionPendingLabel");
                Text status = FindText(view.transform, "CollectionStatusLabel");
                Text title = FindText(view.transform, "CollectionTitleLabel");
                Text itemIdHeader = FindText(view.transform, "ItemIdHeader");
                Text valueHeader = FindText(view.transform, "ValueHeader");
                Button refresh = FindButton(view.transform, "CollectionRefreshButton");
                Transform listRoot = FindTransform(view.transform, "CollectionItemList");
                Assert.That(panel, Is.Not.Null);
                Assert.That(viewport, Is.Not.Null);
                Assert.That(listRoot, Is.Not.Null);

                CollectionPresentationItem[] items =
                {
                    new CollectionPresentationItem(1, 4, 100),
                    new CollectionPresentationItem(2, 1, 300)
                };
                view.Render(new CollectionPresentationSnapshot(
                    CollectionPresentationState.Fresh, items, 1200, 2));
                Canvas.ForceUpdateCanvases();
                yield return null;

                Assert.That(wallet.text, Is.EqualTo("보유 재화  1200"));
                Assert.That(pending.text, Is.EqualTo("정산 대기  2건"));
                Assert.That(status.gameObject.activeSelf, Is.False);
                Assert.That(refresh.interactable, Is.True);
                AssertRectInside(section as RectTransform, panel);
                AssertRectInside(wallet.transform as RectTransform,
                    section as RectTransform);
                AssertRectInside(pending.transform as RectTransform,
                    section as RectTransform);
                AssertRectInside(refresh.transform as RectTransform,
                    section as RectTransform);
                AssertRectInside(viewport, section as RectTransform);
                AssertHorizontalPadding(title.transform as RectTransform,
                    section as RectTransform, 24f);
                AssertHorizontalPadding(itemIdHeader.transform as RectTransform,
                    section as RectTransform, 24f);
                AssertHorizontalPadding(valueHeader.transform as RectTransform,
                    section as RectTransform, 24f);
                AssertHorizontalPadding(refresh.transform as RectTransform,
                    section as RectTransform, 24f);
                AssertHorizontalPadding(viewport, section as RectTransform, 24f);
                foreach (Transform row in ActiveItemRows(listRoot))
                {
                    AssertRectInside(row as RectTransform, viewport);
                    Assert.That((row as RectTransform).rect.width,
                        Is.EqualTo(viewport.rect.width).Within(0.5f));
                }

                view.Render(new CollectionPresentationSnapshot(
                    CollectionPresentationState.Stale, items, 1200, 2));
                Canvas.ForceUpdateCanvases();
                yield return null;

                Assert.That(status.gameObject.activeSelf, Is.True);
                Assert.That(status.text,
                    Is.EqualTo("최근 확인된 컬렉션입니다. 다시 시도해 주세요."));
                Assert.That(refresh.interactable, Is.True);
                AssertRectInside(status.transform as RectTransform,
                    section as RectTransform);
                foreach (Transform row in ActiveItemRows(listRoot))
                {
                    AssertRectInside(row as RectTransform, viewport);
                }
            }
        }

        private static void AssertColumnRows(
            Transform listRoot,
            IReadOnlyList<string> itemIds,
            IReadOnlyList<string> quantities,
            IReadOnlyList<string> values)
        {
            List<Transform> rows = ActiveItemRows(listRoot);
            Assert.That(rows, Has.Count.EqualTo(itemIds.Count));
            for (int index = 0; index < rows.Count; index++)
            {
                Transform row = rows[index];
                Assert.That(row.GetComponent("CollectionItemRowView"), Is.Not.Null);
                Assert.That(FindText(row, "ItemIdLabel").text,
                    Is.EqualTo(itemIds[index]));
                Assert.That(FindText(row, "QuantityLabel").text,
                    Is.EqualTo(quantities[index]));
                Assert.That(FindText(row, "ValueLabel").text,
                    Is.EqualTo(values[index]));
                Assert.That(row.Find("RowSeparator"), Is.Null);
                Assert.That(row.GetComponentsInChildren<Selectable>(true), Is.Empty);
                foreach (Image image in row.GetComponentsInChildren<Image>(true))
                {
                    Assert.That(image.type, Is.EqualTo(Image.Type.Simple));
                    if (image.name != "ItemIcon")
                    {
                        Assert.That(image.sprite, Is.Null);
                    }
                }
            }
        }

        private static List<Transform> ActiveItemRows(Transform listRoot)
        {
            var rows = new List<Transform>();
            foreach (Transform child in listRoot)
            {
                if (child.gameObject.activeSelf &&
                    child.name.StartsWith("CollectionItem-", StringComparison.Ordinal))
                {
                    rows.Add(child);
                }
            }
            return rows;
        }

        private static Transform FindTransform(Transform root, string name)
        {
            foreach (Transform candidate in root.GetComponentsInChildren<Transform>(true))
            {
                if (candidate.name == name)
                {
                    return candidate;
                }
            }
            return null;
        }

        private static Text FindText(Transform root, string name)
        {
            foreach (Text candidate in root.GetComponentsInChildren<Text>(true))
            {
                if (candidate.name == name)
                {
                    return candidate;
                }
            }
            return null;
        }

        private static Image FindImage(Transform root, string name)
        {
            foreach (Image candidate in root.GetComponentsInChildren<Image>(true))
            {
                if (candidate.name == name)
                {
                    return candidate;
                }
            }
            return null;
        }

        private static Button FindButton(Transform root, string name)
        {
            foreach (Button candidate in root.GetComponentsInChildren<Button>(true))
            {
                if (candidate.name == name)
                {
                    return candidate;
                }
            }
            return null;
        }

        private static void AssertRectInside(RectTransform child, RectTransform parent)
        {
            Assert.That(child, Is.Not.Null);
            Assert.That(parent, Is.Not.Null);
            Vector3[] corners = new Vector3[4];
            child.GetWorldCorners(corners);
            foreach (Vector3 corner in corners)
            {
                Vector3 local = parent.InverseTransformPoint(corner);
                Assert.That(local.x, Is.InRange(parent.rect.xMin - 0.5f,
                    parent.rect.xMax + 0.5f));
                Assert.That(local.y, Is.InRange(parent.rect.yMin - 0.5f,
                    parent.rect.yMax + 0.5f));
            }
        }

        private static void AssertHorizontalPadding(
            RectTransform child,
            RectTransform parent,
            float padding)
        {
            Vector3[] corners = new Vector3[4];
            child.GetWorldCorners(corners);
            Vector3 left = parent.InverseTransformPoint(corners[0]);
            Vector3 right = parent.InverseTransformPoint(corners[2]);
            Assert.That(left.x, Is.GreaterThanOrEqualTo(parent.rect.xMin + padding));
            Assert.That(right.x, Is.LessThanOrEqualTo(parent.rect.xMax - padding));
        }

        private static CollectionSnapshot Snapshot(
            ulong quantity,
            ulong wallet,
            long pending)
        {
            return new CollectionSnapshot(
                new[] { new CollectionItem(1, quantity, 100) },
                wallet,
                pending);
        }

        private sealed class RecordingCollectionApi : ICollectionApi
        {
            private readonly CollectionSnapshot snapshot;

            public RecordingCollectionApi(CollectionSnapshot snapshot)
            {
                this.snapshot = snapshot;
            }

            public int Calls { get; private set; }

            public Task<CollectionSnapshot> FetchAsync(CancellationToken cancellationToken)
            {
                Calls++;
                return Task.FromResult(snapshot);
            }
        }

        private sealed class ThrowingCollectionApi : ICollectionApi
        {
            private readonly string message;

            public ThrowingCollectionApi(string message)
            {
                this.message = message;
            }

            public Task<CollectionSnapshot> FetchAsync(CancellationToken cancellationToken)
            {
                throw new InvalidOperationException(message);
            }
        }

        private sealed class DeferredCollectionApi : ICollectionApi
        {
            private readonly TaskCompletionSource<CollectionSnapshot> completion =
                new TaskCompletionSource<CollectionSnapshot>(
                    TaskCreationOptions.RunContinuationsAsynchronously);

            public Task<CollectionSnapshot> FetchAsync(CancellationToken cancellationToken)
            {
                return completion.Task;
            }

            public void Complete(CollectionSnapshot snapshot)
            {
                completion.TrySetResult(snapshot);
            }
        }

        private sealed class FixtureTcpSender : ITcpCommandSender
        {
            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                return Task.CompletedTask;
            }
        }

        private sealed class RecordingCollectionView : ICollectionView
        {
            public List<CollectionPresentationState> States { get; } =
                new List<CollectionPresentationState>();
            public CollectionPresentationSnapshot Last { get; private set; }

            public void Render(CollectionPresentationSnapshot snapshot)
            {
                Last = snapshot;
                States.Add(snapshot.State);
            }
        }
    }
}
