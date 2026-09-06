using System.IO;
using System.Linq;
using LootOfLegends.Presentation.Collection;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Presentation;
using LootOfLegends.Presentation.Common;
using LootOfLegends.Presentation.FinalResult;
using LootOfLegends.Presentation.Lobby;
using LootOfLegends.Presentation.Login;
using LootOfLegends.Presentation.Room;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class PlayerFlowSceneAssetTests
    {
        private const string KenneyInputPath =
            "Assets/ThirdParty/KenneyUIPixelAdventure/large-thick/tile_0001.png";
        private const string KenneyPanelPath =
            "Assets/ThirdParty/KenneyUIPixelAdventure/large-thick/tile_0000.png";
        private const string KenneyListRowPath =
            "Assets/ThirdParty/KenneyUIPixelAdventure/large-thin/tile_0000.png";
        private const string FinalResultPanelPath =
            "Assets/Presentation/Prefabs/Ugui/FinalResultPanel.prefab";
        private const string FinalResultRowPath =
            "Assets/Presentation/Prefabs/Ugui/FinalResultRow.prefab";
        private const string CollectionItemRowPath =
            "Assets/Presentation/Prefabs/Ugui/CollectionItemRow.prefab";

        [TestCase("Assets/Scenes/LoginScene.unity", typeof(LoginScreenView))]
        [TestCase("Assets/Scenes/LobbyScene.unity", typeof(LobbyScreenView))]
        [TestCase("Assets/Scenes/RoomScene.unity", typeof(RoomScreenView))]
        [TestCase("Assets/Scenes/ArenaScene.unity", typeof(ArenaScreenView))]
        public void PresentationSceneLoadsWithoutMissingScripts(string path, System.Type viewType)
        {
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                GameObject[] roots = scene.GetRootGameObjects();
                Assert.That(roots, Is.Not.Empty);
                Assert.That(
                    roots.SelectMany(root => root.GetComponentsInChildren(viewType, true)),
                    Is.Not.Empty);
                Assert.That(
                    roots.Sum(MissingScriptCount),
                    Is.Zero,
                    path + " contains a missing MonoBehaviour script");
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void LoginLobbyAndRoomScenesHaveRequiredUguiBindings()
        {
            AssertUguiBindings<LoginScreenView>(
                "Assets/Scenes/LoginScene.unity",
                "panel",
                "statusView");
            AssertUguiBindings<LobbyScreenView>(
                "Assets/Scenes/LobbyScene.unity",
                "panel",
                "nicknameLabel",
                "statusLabel",
                "roomTitleInput",
                "capacityInput",
                "createButton",
                "roomListRoot",
                "roomButtonPrefab");
            AssertUguiBindings<RoomScreenView>(
                "Assets/Scenes/RoomScene.unity",
                "panel",
                "titleLabel",
                "statusLabel",
                "memberListRoot",
                "memberRowPrefab",
                "readyButton",
                "startButton",
                "leaveButton");
        }

        [Test]
        public void LoginSceneUsesKenneyStatusOnlyModal()
        {
            const string path = "Assets/Scenes/LoginScene.unity";
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                LoginScreenView view = scene.GetRootGameObjects()
                    .SelectMany(root =>
                        root.GetComponentsInChildren<LoginScreenView>(true))
                    .Single();
                var serialized = new SerializedObject(view);
                GameObject panel = (GameObject)serialized.FindProperty("panel")
                    .objectReferenceValue;
                Image panelImage = panel.GetComponent<Image>();
                Assert.That(panelImage.type, Is.EqualTo(Image.Type.Sliced));
                Assert.That(
                    AssetDatabase.GetAssetPath(panelImage.sprite),
                    Is.EqualTo(KenneyPanelPath));

                Text[] labels = panel.GetComponentsInChildren<Text>(true);
                Text heading = labels.SingleOrDefault(
                    label => label.name == "LoginHeading");
                Text provider = labels.SingleOrDefault(
                    label => label.name == "LoginProviderHeading");
                Text notice = labels.SingleOrDefault(
                    label => label.name == "BrowserNotice");
                Assert.That(heading, Is.Not.Null, "LoginHeading is required");
                Assert.That(provider, Is.Not.Null, "LoginProviderHeading is required");
                Assert.That(notice, Is.Not.Null, "BrowserNotice is required");
                Assert.That(heading.text, Is.EqualTo("LOOT OF LEGENDS"));
                Assert.That(provider.text, Is.EqualTo("Google 로그인"));
                Assert.That(notice.text,
                    Is.EqualTo("외부 브라우저에서 인증을 완료해 주세요."));

                LoginStatusTextView statusView = (LoginStatusTextView)serialized
                    .FindProperty("statusView").objectReferenceValue;
                var statusSerialized = new SerializedObject(statusView);
                Text statusLabel = (Text)statusSerialized.FindProperty("label")
                    .objectReferenceValue;
                Assert.That(statusLabel, Is.Not.Null);
                Assert.That(statusLabel.rectTransform.rect.width,
                    Is.GreaterThanOrEqualTo(600f));
                Assert.That(statusLabel.rectTransform.rect.height,
                    Is.GreaterThanOrEqualTo(80f));
                Assert.That(view.GetComponentsInChildren<Button>(true), Is.Empty,
                    "Login remains a status-only surface without a product command");
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void RoomSceneUsesKenneyRowsAndActionHierarchy()
        {
            const string path = "Assets/Scenes/RoomScene.unity";
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                RoomScreenView view = scene.GetRootGameObjects()
                    .SelectMany(root => root.GetComponentsInChildren<RoomScreenView>(true))
                    .Single();
                var serialized = new SerializedObject(view);
                GameObject panel = (GameObject)serialized.FindProperty("panel")
                    .objectReferenceValue;
                GameObject row = (GameObject)serialized.FindProperty("memberRowPrefab")
                    .objectReferenceValue;
                Assert.That(
                    AssetDatabase.GetAssetPath(panel.GetComponent<Image>().sprite),
                    Is.EqualTo(KenneyPanelPath));
                Assert.That(
                    AssetDatabase.GetAssetPath(row.GetComponent<Image>().sprite),
                    Is.EqualTo(KenneyListRowPath));
                Assert.That(
                    AssetDatabase.GetAssetPath(row.GetComponent<Image>().sprite),
                    Is.Not.EqualTo(KenneyPanelPath));

                Button ready = (Button)serialized.FindProperty("readyButton")
                    .objectReferenceValue;
                Button start = (Button)serialized.FindProperty("startButton")
                    .objectReferenceValue;
                Button leave = (Button)serialized.FindProperty("leaveButton")
                    .objectReferenceValue;
                Assert.That(start.GetComponent<RectTransform>().rect.width,
                    Is.GreaterThan(ready.GetComponent<RectTransform>().rect.width));
                Assert.That(ready.GetComponent<RectTransform>().rect.width,
                    Is.GreaterThan(leave.GetComponent<RectTransform>().rect.width));
                Assert.That(start.GetComponent<RectTransform>().rect.height,
                    Is.GreaterThan(ready.GetComponent<RectTransform>().rect.height));
                Assert.That(ready.GetComponent<RectTransform>().rect.height,
                    Is.GreaterThanOrEqualTo(54f));
                Assert.That(leave.GetComponent<RectTransform>().rect.height,
                    Is.GreaterThanOrEqualTo(54f));
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void LobbySceneUsesKenneyInputSkinAtReferenceResolution()
        {
            const string path = "Assets/Scenes/LobbyScene.unity";
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                GameObject[] roots = scene.GetRootGameObjects();
                LobbyScreenView view = roots
                    .SelectMany(root => root.GetComponentsInChildren<LobbyScreenView>(true))
                    .Single();
                var serialized = new SerializedObject(view);
                InputField input = (InputField)serialized
                    .FindProperty("roomTitleInput").objectReferenceValue;
                Image image = input.GetComponent<Image>();
                Assert.That(image.type, Is.EqualTo(Image.Type.Sliced));
                Assert.That(
                    AssetDatabase.GetAssetPath(image.sprite),
                    Is.EqualTo(KenneyInputPath));

                CanvasScaler scaler = roots
                    .SelectMany(root => root.GetComponentsInChildren<CanvasScaler>(true))
                    .Single(candidate =>
                        candidate.GetComponent<SafeFailureTextView>() == null);
                Assert.That(
                    scaler.uiScaleMode,
                    Is.EqualTo(CanvasScaler.ScaleMode.ScaleWithScreenSize));
                Assert.That(
                    scaler.referenceResolution,
                    Is.EqualTo(new Vector2(1920f, 1080f)));
                Assert.That(scaler.matchWidthOrHeight, Is.EqualTo(0.5f));
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void ArenaAndCollectionScenesHaveRequiredUguiBindings()
        {
            AssertUguiBindings<ArenaWorldView>(
                "Assets/Scenes/ArenaScene.unity",
                "catalog",
                "audioSource",
                "playerPrefab",
                "monsterPrefab",
                "dropPrefab");
            AssertUguiBindings<ArenaScreenView>(
                "Assets/Scenes/ArenaScene.unity",
                "panel",
                "statusLabel",
                "monsterHudPanel",
                "monsterHitPointsLabel",
                "monsterHitPointsFill",
                "transientStatusLabel",
                "terminalCopyLabel",
                "lootCountLabel",
                "inputHintLabel",
                "attackHintLabel",
                "claimHintLabel",
                "worldView",
                "waitingOverlay");
            AssertUguiBindings<FinalResultScreenView>(
                "Assets/Scenes/ArenaScene.unity",
                "panel",
                "headingLabel",
                "outcomeLabel",
                "resultListRoot",
                "resultRowPrefab",
                "rematchLabel",
                "catalog",
                "audioSource");
            AssertUguiBindings<CollectionScreenView>(
                "Assets/Scenes/LobbyScene.unity",
                "panel",
                "panelGroup",
                "toggleButton",
                "walletLabel",
                "pendingLabel",
                "statusLabel",
                "emptyLabel",
                "itemListRoot",
                "itemRowPrefab",
                "refreshButton");
        }

        [Test]
        public void LobbyCollectionUsesScrollableColumnRowsWithoutNewCommands()
        {
            const string path = "Assets/Scenes/LobbyScene.unity";
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                CollectionScreenView view = scene.GetRootGameObjects()
                    .SelectMany(root =>
                        root.GetComponentsInChildren<CollectionScreenView>(true))
                    .Single();
                Assert.That(view.transform.parent.name, Is.EqualTo("LobbyPanel"));

                var serialized = new SerializedObject(view);
                CanvasGroup panelGroup = (CanvasGroup)serialized
                    .FindProperty("panelGroup").objectReferenceValue;
                Button toggle = (Button)serialized.FindProperty("toggleButton")
                    .objectReferenceValue;
                Transform listRoot = (Transform)serialized.FindProperty("itemListRoot")
                    .objectReferenceValue;
                GameObject row = (GameObject)serialized.FindProperty("itemRowPrefab")
                    .objectReferenceValue;
                Button refresh = (Button)serialized.FindProperty("refreshButton")
                    .objectReferenceValue;
                Assert.That(AssetDatabase.GetAssetPath(row),
                    Is.EqualTo(CollectionItemRowPath));
                Assert.That(panelGroup, Is.Not.Null);
                Assert.That(toggle, Is.Not.Null);
                Assert.That(toggle.name, Is.EqualTo("CollectionToggleButton"));
                Assert.That(toggle.transform.IsChildOf(view.transform), Is.False,
                    "the Lobby toggle must remain usable while the modal is hidden");
                Image modalImage = view.GetComponent<Image>();
                Assert.That(modalImage, Is.Not.Null);
                Assert.That(modalImage.type, Is.EqualTo(Image.Type.Sliced));
                Assert.That(AssetDatabase.GetAssetPath(modalImage.sprite),
                    Is.EqualTo(KenneyPanelPath));
                Assert.That(modalImage.raycastTarget, Is.True);
                CollectionItemRowView rowView = row.GetComponent<CollectionItemRowView>();
                Assert.That(rowView, Is.Not.Null);
                Assert.That(row.GetComponentsInChildren<Selectable>(true), Is.Empty);
                var rowSerialized = new SerializedObject(rowView);
                SerializedProperty catalogProperty = rowSerialized.FindProperty("catalog");
                SerializedProperty itemIconProperty = rowSerialized.FindProperty("itemIcon");
                SerializedProperty itemIdLabelProperty = rowSerialized
                    .FindProperty("itemIdLabel");
                Assert.That(catalogProperty, Is.Not.Null);
                Assert.That(itemIconProperty, Is.Not.Null);
                Assert.That(itemIdLabelProperty, Is.Not.Null);
                PresentationCatalog catalog = (PresentationCatalog)catalogProperty
                    .objectReferenceValue;
                Image itemIcon = (Image)itemIconProperty.objectReferenceValue;
                Assert.That(catalog, Is.Not.Null);
                Assert.That(AssetDatabase.GetAssetPath(catalog),
                    Is.EqualTo("Assets/Presentation/PresentationCatalog.asset"));
                Assert.That(itemIcon, Is.Not.Null);
                Assert.That(itemIcon.name, Is.EqualTo("ItemIcon"));
                Assert.That(itemIcon.type, Is.EqualTo(Image.Type.Simple));
                Assert.That(itemIcon.raycastTarget, Is.False);
                foreach (string labelName in new[]
                         {
                             "ItemIdLabel", "QuantityLabel", "ValueLabel"
                         })
                {
                    Assert.That(row.GetComponentsInChildren<Transform>(true)
                        .Any(child => child.name == labelName), Is.True, labelName);
                }
                Assert.That(itemIdLabelProperty.objectReferenceValue, Is.Not.Null);
                Assert.That(row.transform.Find("RowSeparator"), Is.Null);

                ScrollRect scroll = listRoot.GetComponentInParent<ScrollRect>();
                Assert.That(scroll, Is.Not.Null);
                Assert.That(scroll.content, Is.EqualTo(listRoot as RectTransform));
                Assert.That(scroll.vertical, Is.True);
                Assert.That(scroll.horizontal, Is.False);
                Assert.That(scroll.verticalScrollbar, Is.Null);
                Assert.That(view.GetComponentsInChildren<Button>(true),
                    Has.Length.EqualTo(1));
                Assert.That(refresh.name, Is.EqualTo("CollectionRefreshButton"));
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void LobbyRoomListUsesDedicatedVerticalScrollViewport()
        {
            const string path = "Assets/Scenes/LobbyScene.unity";
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                LobbyScreenView view = scene.GetRootGameObjects()
                    .SelectMany(root =>
                        root.GetComponentsInChildren<LobbyScreenView>(true))
                    .Single();
                var serialized = new SerializedObject(view);
                Transform listRoot = (Transform)serialized.FindProperty("roomListRoot")
                    .objectReferenceValue;
                ScrollRect scroll = listRoot.GetComponentInParent<ScrollRect>();

                Assert.That(scroll, Is.Not.Null);
                Assert.That(scroll.content, Is.EqualTo(listRoot as RectTransform));
                Assert.That(scroll.viewport, Is.Not.Null);
                Assert.That(scroll.viewport.GetComponent<RectMask2D>(), Is.Not.Null);
                Assert.That(scroll.vertical, Is.True);
                Assert.That(scroll.horizontal, Is.False);
                Assert.That(scroll.verticalScrollbar, Is.Null);
                Assert.That(listRoot.GetComponent<ContentSizeFitter>(), Is.Not.Null);
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void ArenaUsesKeyboardHintWithoutClickControls()
        {
            const string path = "Assets/Scenes/ArenaScene.unity";
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                GameObject[] roots = scene.GetRootGameObjects();
                Assert.That(
                    roots.SelectMany(root => root.GetComponentsInChildren<Button>(true))
                        .Select(button => button.name),
                    Is.EquivalentTo(new[]
                    {
                        "FinalResultRoomButton",
                        "FinalResultLobbyButton"
                    }));
                ArenaScreenView view = roots.SelectMany(root =>
                        root.GetComponentsInChildren<ArenaScreenView>(true))
                    .Single();
                var serialized = new SerializedObject(view);
                foreach (string removed in new[]
                         {
                             "moveLeftButton",
                             "moveUpButton",
                             "moveDownButton",
                             "moveRightButton",
                             "attackButton",
                             "claimButton"
                         })
                {
                    Assert.That(serialized.FindProperty(removed), Is.Null, removed);
                }
                Text hint = (Text)serialized.FindProperty("inputHintLabel")
                    .objectReferenceValue;
                // 키 이름은 이제 키캡 아트가 담는다. 라벨은 동작만 말한다.
                Assert.That(hint.text, Is.EqualTo("이동"));
                Assert.That(
                    ((Text)serialized.FindProperty("attackHintLabel").objectReferenceValue).text,
                    Is.EqualTo("공격"));
                Assert.That(
                    ((Text)serialized.FindProperty("claimHintLabel").objectReferenceValue).text,
                    Is.EqualTo("획득"));
                Image hintPanel = hint.GetComponentInParent<Image>();
                Assert.That(hintPanel.type, Is.EqualTo(Image.Type.Sliced));
                Assert.That(
                    AssetDatabase.GetAssetPath(hintPanel.sprite),
                    Is.EqualTo(KenneyListRowPath));
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void FinalResultUsesDedicatedKenneyOverlayAndTenRowLayout()
        {
            const string path = "Assets/Scenes/ArenaScene.unity";
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                FinalResultScreenView view = scene.GetRootGameObjects()
                    .SelectMany(root =>
                        root.GetComponentsInChildren<FinalResultScreenView>(true))
                    .Single();
                var serialized = new SerializedObject(view);
                GameObject panel = (GameObject)serialized.FindProperty("panel")
                    .objectReferenceValue;
                Transform listRoot = (Transform)serialized.FindProperty("resultListRoot")
                    .objectReferenceValue;
                GameObject row = (GameObject)serialized.FindProperty("resultRowPrefab")
                    .objectReferenceValue;

                Assert.That(panel.activeSelf, Is.False);
                Assert.That(
                    PrefabUtility.GetPrefabAssetPathOfNearestInstanceRoot(panel),
                    Is.EqualTo(FinalResultPanelPath));
                Assert.That(AssetDatabase.GetAssetPath(row), Is.EqualTo(FinalResultRowPath));
                Assert.That(
                    panel.GetComponentsInChildren<Button>(true)
                        .Select(button => button.name),
                    Is.EquivalentTo(new[]
                    {
                        "FinalResultRoomButton",
                        "FinalResultLobbyButton"
                    }));
                Assert.That(row.GetComponentsInChildren<Selectable>(true), Is.Empty);
                Assert.That(
                    panel.GetComponentsInChildren<Image>(true)
                        .Any(image => image.name == "FinalResultDimLayer"),
                    Is.True);
                Image modal = panel.GetComponentsInChildren<Image>(true)
                    .Single(image => image.name == "FinalResultModal");
                Assert.That(modal.type, Is.EqualTo(Image.Type.Sliced));
                Assert.That(AssetDatabase.GetAssetPath(modal.sprite),
                    Is.EqualTo(KenneyPanelPath));

                foreach (string labelName in new[]
                         {
                             "TopLabel", "RankLabel", "NicknameLabel", "AssetValueLabel"
                         })
                {
                    Assert.That(
                        row.GetComponentsInChildren<Text>(true)
                            .Any(label => label.name == labelName),
                        Is.True,
                        labelName);
                }
                Assert.That(row.GetComponent("FinalResultRowView"), Is.Not.Null);

                Image rowBackground = row.GetComponent<Image>();
                Assert.That(rowBackground, Is.Not.Null);
                Assert.That(rowBackground.sprite, Is.Null);
                Assert.That(rowBackground.type, Is.EqualTo(Image.Type.Simple));
                Image separator = row.GetComponentsInChildren<Image>(true)
                    .Single(image => image.name == "RowSeparator");
                Assert.That(separator.sprite, Is.Null);
                Assert.That(separator.type, Is.EqualTo(Image.Type.Simple));
                Assert.That(separator.GetComponent<RectTransform>().rect.height,
                    Is.InRange(1f, 2f));

                foreach ((string headerName, string labelName) in new[]
                         {
                             ("RankHeader", "RankLabel"),
                             ("NicknameHeader", "NicknameLabel"),
                             ("AssetValueHeader", "AssetValueLabel")
                         })
                {
                    RectTransform header = panel.GetComponentsInChildren<RectTransform>(true)
                        .Single(rect => rect.name == headerName);
                    RectTransform label = row.GetComponentsInChildren<RectTransform>(true)
                        .Single(rect => rect.name == labelName);
                    Assert.That(label.anchorMin, Is.EqualTo(header.anchorMin), labelName);
                    Assert.That(label.anchorMax, Is.EqualTo(header.anchorMax), labelName);
                    Assert.That(label.anchoredPosition,
                        Is.EqualTo(header.anchoredPosition), labelName);
                    Assert.That(label.sizeDelta, Is.EqualTo(header.sizeDelta), labelName);
                }

                var layout = listRoot.GetComponent<VerticalLayoutGroup>();
                Assert.That(layout, Is.Not.Null);
                float rowHeight = row.GetComponent<RectTransform>().rect.height;
                float requiredHeight = rowHeight * 10f + layout.spacing * 9f;
                Assert.That(listRoot.GetComponent<RectTransform>().rect.height,
                    Is.GreaterThanOrEqualTo(requiredHeight));
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void ArenaSceneFramesBoundsAndWiresWaitingOverlay()
        {
            const string path = "Assets/Scenes/ArenaScene.unity";
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                GameObject[] roots = scene.GetRootGameObjects();
                Camera camera = roots.SelectMany(root =>
                        root.GetComponentsInChildren<Camera>(true))
                    .Single();
                Assert.That(camera.orthographic, Is.True);
                Assert.That(camera.orthographicSize, Is.GreaterThanOrEqualTo(10.5f));
                Assert.That(camera.orthographicSize * 16f / 9f,
                    Is.GreaterThanOrEqualTo(10.5f));
                Assert.That(camera.GetComponents<Component>().Any(component =>
                    component != null && component.GetType().Namespace != null &&
                    component.GetType().Namespace.StartsWith("Cinemachine")), Is.False);

                BattleLoadWaitingOverlay overlay = roots.SelectMany(root =>
                        root.GetComponentsInChildren<BattleLoadWaitingOverlay>(true))
                    .Single();
                var serialized = new SerializedObject(overlay);
                GameObject panel = (GameObject)serialized.FindProperty("panel")
                    .objectReferenceValue;
                Text title = (Text)serialized.FindProperty("titleLabel")
                    .objectReferenceValue;
                Text description = (Text)serialized.FindProperty("descriptionLabel")
                    .objectReferenceValue;
                Assert.That(overlay.GetComponent<CanvasGroup>(), Is.Not.Null);
                Assert.That(panel, Is.Not.Null);
                Assert.That(title.text, Is.EqualTo("전투 준비 중"));
                Assert.That(description.text,
                    Is.EqualTo("서버 전투 시작을 기다리고 있습니다."));
                Assert.That(overlay.GetComponentsInChildren<Button>(true), Is.Empty);
                Assert.That(
                    roots.SelectMany(root => root.GetComponentsInChildren<Button>(true))
                        .Select(button => button.name),
                    Is.EquivalentTo(new[]
                    {
                        "FinalResultRoomButton",
                        "FinalResultLobbyButton"
                    }));
                Assert.That(roots.Any(root => root.name == "ArenaBackdrop"), Is.True);

                string runtime = File.ReadAllText(Path.Combine(
                    Application.dataPath,
                    "LootOfLegends/Presentation/PlayerFlowPresentationRuntime.cs"));
                string bootstrap = File.ReadAllText(Path.Combine(
                    Application.dataPath,
                    "LootOfLegends/Bootstrap/ProductPlayerFlowBootstrap.cs"));
                StringAssert.DoesNotContain(nameof(BattleLoadWaitingOverlay), runtime);
                StringAssert.DoesNotContain(nameof(BattleLoadWaitingOverlay), bootstrap);
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void ArenaPlayerPrefabOwnsOnlyItsSpritePresentation()
        {
            GameObject player = AssetDatabase.LoadAssetAtPath<GameObject>(
                "Assets/Presentation/Prefabs/World/ArenaPlayer.prefab");
            Assert.That(player, Is.Not.Null);
            Assert.That(player.GetComponent<SpriteRenderer>(), Is.Not.Null);
            Assert.That(player.GetComponent<ArenaPlayerVisual>(), Is.Not.Null);
        }

        [TestCase("Assets/Scenes/LoginScene.unity")]
        [TestCase("Assets/Scenes/LobbyScene.unity")]
        [TestCase("Assets/Scenes/RoomScene.unity")]
        [TestCase("Assets/Scenes/ArenaScene.unity")]
        public void SceneContainsBoundSafeFailureOverlay(string path)
        {
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                SafeFailureTextView view = scene.GetRootGameObjects()
                    .SelectMany(root =>
                        root.GetComponentsInChildren<SafeFailureTextView>(true))
                    .Single();
                Assert.That(view.GetComponent<Canvas>(), Is.Not.Null, path);
                Assert.That(view.GetComponent<CanvasScaler>(), Is.Not.Null, path);
                Assert.That(view.GetComponent<GraphicRaycaster>(), Is.Not.Null, path);
                var serialized = new SerializedObject(view);
                Assert.That(serialized.FindProperty("panel").objectReferenceValue,
                    Is.Not.Null, path);
                Assert.That(serialized.FindProperty("label").objectReferenceValue,
                    Is.Not.Null, path);
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [TestCase("Assets/Scenes/LoginScene.unity")]
        [TestCase("Assets/Scenes/LobbyScene.unity")]
        [TestCase("Assets/Scenes/RoomScene.unity")]
        [TestCase("Assets/Scenes/ArenaScene.unity")]
        public void SmallDesktopViewportKeepsReadableTextAndClickTargets(string path)
        {
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                GameObject[] roots = scene.GetRootGameObjects();
                foreach (Text label in roots.SelectMany(root =>
                             root.GetComponentsInChildren<Text>(true)))
                {
                    Assert.That(label.fontSize, Is.GreaterThanOrEqualTo(24),
                        path + " " + label.name);
                }
                foreach (Selectable selectable in roots.SelectMany(root =>
                             root.GetComponentsInChildren<Selectable>(true)))
                {
                    Assert.That(selectable.GetComponent<RectTransform>().rect.height,
                        Is.GreaterThanOrEqualTo(54f),
                        path + " " + selectable.name);
                }
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void RuntimeWiresOnlyProductScreenViews()
        {
            string source = File.ReadAllText(Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/PlayerFlowPresentationRuntime.cs"));

            foreach (string type in new[]
                     {
                         nameof(LobbyScreenView),
                         nameof(RoomScreenView),
                         nameof(ArenaScreenView),
                         nameof(FinalResultScreenView),
                         nameof(CollectionScreenView)
                     })
            {
                StringAssert.Contains("FindFirstObjectByType<" + type + ">", source);
            }
            foreach (string type in new[]
                     {
                         "LobbyTextView",
                         "RoomTextView",
                         "ArenaTextView",
                         "FinalResultTextView",
                         "CollectionTextView"
                     })
            {
                StringAssert.DoesNotContain("FindFirstObjectByType<" + type + ">", source);
            }
        }

        private static void AssertUguiBindings<T>(
            string path,
            params string[] propertyNames)
            where T : Component
        {
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                GameObject[] roots = scene.GetRootGameObjects();
                Assert.That(
                    roots.SelectMany(root => root.GetComponentsInChildren<Canvas>(true))
                        .Where(canvas =>
                            canvas.GetComponent<SafeFailureTextView>() == null)
                        .ToArray(),
                    Has.Length.EqualTo(1),
                    path);
                Assert.That(
                    roots.SelectMany(root =>
                            root.GetComponentsInChildren<CanvasScaler>(true))
                        .Where(scaler =>
                            scaler.GetComponent<SafeFailureTextView>() == null)
                        .ToArray(),
                    Has.Length.EqualTo(1),
                    path);
                Assert.That(
                    roots.SelectMany(root =>
                            root.GetComponentsInChildren<GraphicRaycaster>(true))
                        .Where(raycaster =>
                            raycaster.GetComponent<SafeFailureTextView>() == null)
                        .ToArray(),
                    Has.Length.EqualTo(1),
                    path);
                Assert.That(
                    roots.SelectMany(root =>
                            root.GetComponentsInChildren<EventSystem>(true))
                        .ToArray(),
                    Has.Length.EqualTo(1),
                    path);

                T view = roots
                    .SelectMany(root => root.GetComponentsInChildren<T>(true))
                    .Single();
                var serialized = new SerializedObject(view);
                foreach (string propertyName in propertyNames)
                {
                    SerializedProperty property = serialized.FindProperty(propertyName);
                    Assert.That(property, Is.Not.Null, typeof(T).Name + "." + propertyName);
                    Assert.That(
                        property.objectReferenceValue,
                        Is.Not.Null,
                        typeof(T).Name + "." + propertyName);
                }
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        private static int MissingScriptCount(GameObject root)
        {
            int count = 0;
            foreach (Transform child in root.GetComponentsInChildren<Transform>(true))
            {
                count += GameObjectUtility.GetMonoBehavioursWithMissingScriptCount(
                    child.gameObject);
            }
            return count;
        }
    }
}
