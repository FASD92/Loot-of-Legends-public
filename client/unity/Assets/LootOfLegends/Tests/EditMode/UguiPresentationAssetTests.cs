using System;
using System.IO;
using System.Linq;
using LootOfLegends.Presentation.Arena;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.Tilemaps;
using UnityEngine.UI;
using Object = UnityEngine.Object;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class UguiPresentationAssetTests
    {
        private const string KenneyRoot =
            "Assets/ThirdParty/KenneyUIPixelAdventure/";
        private const string PresentationPackRoot =
            "Assets/ThirdParty/PresentationPack/";

        [Test]
        public void VisualPrefabsContainRenderableUguiComponents()
        {
            GameObject panel = LoadPrefab("Assets/Presentation/Prefabs/Ugui/Panel.prefab");
            Image panelImage = panel.GetComponent<Image>();
            Assert.That(panelImage, Is.Not.Null);
            Assert.That(panelImage.sprite, Is.Not.Null);
            Assert.That(panelImage.type, Is.EqualTo(Image.Type.Sliced));
            AssertAssetPath(panelImage.sprite, "large-thick/tile_0000.png");

            GameObject buttonObject = LoadPrefab(
                "Assets/Presentation/Prefabs/Ugui/PrimaryButton.prefab");
            Image buttonImage = buttonObject.GetComponent<Image>();
            Assert.That(buttonImage.type, Is.EqualTo(Image.Type.Sliced));
            Button button = buttonObject.GetComponent<Button>();
            Assert.That(button, Is.Not.Null);
            Assert.That(button.targetGraphic, Is.SameAs(buttonImage));
            Assert.That(button.transition, Is.EqualTo(Selectable.Transition.SpriteSwap));
            AssertAssetPath(buttonImage.sprite, "large-thick/tile_0001.png");
            AssertAssetPath(
                button.spriteState.highlightedSprite,
                "large-thin/tile_0001.png");
            AssertAssetPath(
                button.spriteState.pressedSprite,
                "large-thick/tile_0014.png");
            AssertAssetPath(
                button.spriteState.disabledSprite,
                "large-thin/tile_0000.png");
            Component sound = buttonObject.GetComponent("UiButtonSound");
            Assert.That(sound, Is.Not.Null);
            AudioSource audioSource = buttonObject.GetComponent<AudioSource>();
            Assert.That(audioSource, Is.Not.Null);
            Assert.That(audioSource.playOnAwake, Is.False);
            Assert.That(audioSource.loop, Is.False);
            Assert.That(audioSource.spatialBlend, Is.Zero);

            var soundObject = new SerializedObject(sound);
            Assert.That(soundObject.FindProperty("catalog").objectReferenceValue,
                Is.EqualTo(AssetDatabase.LoadAssetAtPath<Object>(
                    "Assets/Presentation/PresentationCatalog.asset")));
            Assert.That(soundObject.FindProperty("audioSource").objectReferenceValue,
                Is.SameAs(audioSource));

            GameObject row = LoadPrefab("Assets/Presentation/Prefabs/Ugui/ListRow.prefab");
            Image rowImage = row.GetComponent<Image>();
            Assert.That(rowImage.type, Is.EqualTo(Image.Type.Sliced));
            AssertAssetPath(rowImage.sprite, "large-thin/tile_0000.png");
            Assert.That(row.GetComponentInChildren<Text>(true), Is.Not.Null);
        }

        [Test]
        public void PixelUiSpritesUseExactImporterContract()
        {
            AssertSpriteImporter(
                KenneyRoot + "large-thick/tile_0000.png",
                new Vector4(5f, 5f, 5f, 5f));
            AssertSpriteImporter(
                KenneyRoot + "large-thick/tile_0001.png",
                new Vector4(5f, 5f, 5f, 5f));
            AssertSpriteImporter(
                KenneyRoot + "large-thick/tile_0014.png",
                new Vector4(5f, 5f, 5f, 5f));
            AssertSpriteImporter(
                KenneyRoot + "large-thin/tile_0000.png",
                new Vector4(4f, 4f, 4f, 4f));
            AssertSpriteImporter(
                KenneyRoot + "large-thin/tile_0001.png",
                new Vector4(3f, 3f, 3f, 3f));
            AssertSpriteImporter(
                KenneyRoot + "large-thin/tile_0014.png",
                new Vector4(3f, 3f, 3f, 3f));
        }

        [Test]
        public void VillageBackdropUsesTilemapsAndProductOwnedTiles()
        {
            GameObject backdrop = LoadPrefab(
                "Assets/Presentation/Prefabs/World/VillageBackdrop.prefab");
            Tilemap[] tilemaps = backdrop.GetComponentsInChildren<Tilemap>(true);
            Assert.That(tilemaps, Has.Length.GreaterThanOrEqualTo(2));
            Assert.That(backdrop.GetComponentInChildren<RawImage>(true), Is.Null);

            foreach (Tilemap tilemap in tilemaps)
            {
                Assert.That(tilemap.GetComponent<TilemapRenderer>(), Is.Not.Null);
                Assert.That(tilemap.GetUsedTilesCount(), Is.GreaterThan(0), tilemap.name);
            }

            Tile ground = AssetDatabase.LoadAssetAtPath<Tile>(
                "Assets/Presentation/Tiles/Ground.asset");
            Tile path = AssetDatabase.LoadAssetAtPath<Tile>(
                "Assets/Presentation/Tiles/Path.asset");
            // 아틀라스 tileset_floor.png 는 352x417 이고 세로가 16 의 배수가 아니다. 스프라이트
            // rect 는 바닥 기준 16 격자에 정렬되므로, 위쪽 기준으로 그려진 아트와 세로로 1픽셀
            // 어긋난다. y 를 208/240 으로 두면 각 타일이 아래 이웃 타일의 한 줄을 물어서 타일
            // 행마다 어두운 가로줄이 생긴다. 209/241 이 아트 격자와 맞는 값이다.
            Assert.That(ground.sprite.rect,
                Is.EqualTo(new Rect(64f, 209f, 16f, 16f)));
            Assert.That(path.sprite.rect,
                Is.EqualTo(new Rect(128f, 241f, 16f, 16f)));

            foreach (Tilemap tilemap in tilemaps)
            {
                var used = new TileBase[tilemap.GetUsedTilesCount()];
                int count = tilemap.GetUsedTilesNonAlloc(used);
                Assert.That(
                    used.Take(count).All(tile => tile == ground || tile == path),
                    Is.True,
                    tilemap.name);
            }
            Assert.That(AssetDatabase.LoadAssetAtPath<Tile>(
                "Assets/Presentation/Tiles/House.asset"), Is.Null);
            Assert.That(AssetDatabase.LoadAssetAtPath<Tile>(
                "Assets/Presentation/Tiles/Tree.asset"), Is.Null);
        }

        [Test]
        public void ArenaVisualPrefabsUseKenneyHudAndUniformEntityScale()
        {
            GameObject hud = LoadPrefab(
                "Assets/Presentation/Prefabs/Ugui/ArenaHudPanel.prefab");
            GameObject waiting = LoadPrefab(
                "Assets/Presentation/Prefabs/Ugui/ArenaWaitingPanel.prefab");
            GameObject backdrop = LoadPrefab(
                "Assets/Presentation/Prefabs/World/ArenaBackdrop.prefab");

            foreach (GameObject prefab in new[] { hud, waiting })
            {
                Assert.That(prefab.GetComponentsInChildren<Button>(true), Is.Empty);
                foreach (Image image in prefab.GetComponentsInChildren<Image>(true)
                             .Where(image => image.sprite != null))
                {
                    // 두 CC0 vendor 루트만 허용한다. 프레임과 콘텐츠 글리프는 서로 다른 팩에서
                    // 오지만, 그 밖의 경로가 섞이면 톤이 어긋나므로 여기서 막는다.
                    string path = AssetDatabase.GetAssetPath(image.sprite);
                    Assert.That(
                        path.StartsWith(KenneyRoot, StringComparison.Ordinal) ||
                        path.StartsWith(PresentationPackRoot, StringComparison.Ordinal),
                        Is.True,
                        $"{image.name} -> {path}");

                    // Image type 을 스프라이트의 실제 9슬라이스 border 에 묶는다. border 가 있는
                    // 스프라이트를 Simple 로 그리면 테두리가 늘어나고, border 가 없는 스프라이트를
                    // Sliced 로 그리면 아무 효과 없이 draw call 만 늘어난다. 둘 다 실제로 나는
                    // 결함이라 "전부 Sliced" 보다 이 규칙이 잡아내는 범위가 넓다.
                    bool nineSliced = image.sprite.border != Vector4.zero;
                    Assert.That(
                        image.type,
                        Is.EqualTo(nineSliced ? Image.Type.Sliced : Image.Type.Simple),
                        $"{image.name} border={image.sprite.border}");
                }
            }

            // 패널과 프레임은 계속 Kenney 한 팩으로만 스킨한다. 이름이 Panel 로 끝나는 Image 가
            // 그 경계이고, 여기에 다른 팩이 섞이면 창틀 모양이 화면마다 달라진다.
            foreach (Image frame in hud.GetComponentsInChildren<Image>(true)
                         .Where(image => image.sprite != null)
                         .Where(image => image.name.EndsWith("Panel", StringComparison.Ordinal)))
            {
                StringAssert.StartsWith(
                    KenneyRoot,
                    AssetDatabase.GetAssetPath(frame.sprite),
                    frame.name);
                Assert.That(frame.type, Is.EqualTo(Image.Type.Sliced), frame.name);
            }

            Tilemap[] tilemaps = backdrop.GetComponentsInChildren<Tilemap>(true);
            Assert.That(tilemaps, Has.Length.EqualTo(2));
            Assert.That(tilemaps.All(tilemap => tilemap.GetUsedTilesCount() > 0), Is.True);

            AssertUniformScale(
                "Assets/Presentation/Prefabs/World/ArenaPlayer.prefab", 1f);
            AssertUniformScale(
                "Assets/Presentation/Prefabs/World/ArenaMonster.prefab", 1.4f);
            AssertUniformScale(
                "Assets/Presentation/Prefabs/World/ArenaDrop.prefab", 0.9f);
        }

        [Test]
        public void ArenaInputHintShowsAKeyboardCapForEveryControl()
        {
            GameObject hud = LoadPrefab(
                "Assets/Presentation/Prefabs/Ugui/ArenaHudPanel.prefab");
            Transform panel = hud.GetComponentsInChildren<Transform>(true)
                .Single(transform => transform.name == "ArenaInputHintPanel");

            // 이동 4개 + 공격 + 획득. 캡 아트가 키 이름을 담으므로 라벨은 동작만 말한다.
            foreach ((string capName, string spritePath) in new[]
                     {
                         ("KeyCapW", "content/ui/input/key_w.png"),
                         ("KeyCapA", "content/ui/input/key_a.png"),
                         ("KeyCapS", "content/ui/input/key_s.png"),
                         ("KeyCapD", "content/ui/input/key_d.png"),
                         ("KeyCapSpace", "content/ui/input/key_space.png"),
                         ("KeyCapE", "content/ui/input/key_e.png")
                     })
            {
                Image cap = panel.GetComponentsInChildren<Image>(true)
                    .Single(image => image.name == capName);
                Assert.That(
                    AssetDatabase.GetAssetPath(cap.sprite),
                    Is.EqualTo(PresentationPackRoot + spritePath),
                    capName);
                Assert.That(cap.type, Is.EqualTo(Image.Type.Simple), capName);
                Assert.That(cap.sprite.border, Is.EqualTo(Vector4.zero), capName);

                // 정수 배율만 허용한다. 13픽셀 캡을 비정수 배율로 늘리면 픽셀아트가 뭉개져서
                // 텍스트로 두는 것보다 나빠진다.
                var element = cap.GetComponent<LayoutElement>();
                Assert.That(element, Is.Not.Null, capName);
                float scale = element.preferredHeight / cap.sprite.rect.height;
                Assert.That(scale, Is.EqualTo(Mathf.Round(scale)).Within(0.001f), capName);
                Assert.That(
                    element.preferredWidth,
                    Is.EqualTo(cap.sprite.rect.width * scale).Within(0.001f),
                    capName);
            }

            // 캡과 라벨이 형제라서 ArenaScreenView 는 패널 전체를 토글해야 한다. 라벨이 한 단계
            // 더 깊어지면 토글 대상이 그룹으로 바뀌어 힌트가 사라지지 않는다.
            foreach (string labelName in new[]
                     { "InputHintLabel", "AttackHintLabel", "ClaimHintLabel" })
            {
                Transform label = panel.GetComponentsInChildren<Text>(true)
                    .Single(text => text.name == labelName).transform;
                Assert.That(label.parent, Is.SameAs(panel), labelName);
            }
            Assert.That(panel.GetComponent<HorizontalLayoutGroup>(), Is.Not.Null);
        }

        [Test]
        public void ArenaInputHintAdvertisesPortfolioReconnectShortcut()
        {
            GameObject hud = LoadPrefab(
                "Assets/Presentation/Prefabs/Ugui/ArenaHudPanel.prefab");
            Transform panel = hud.GetComponentsInChildren<Transform>(true)
                .Single(transform => transform.name == "ArenaInputHintPanel");
            Text[] labels = panel.GetComponentsInChildren<Text>(true);

            Assert.That(
                labels.Single(label => label.name == "KeyCapF8").text,
                Is.EqualTo("[F8]"));
            Assert.That(
                labels.Single(label => label.name == "ReconnectHintLabel").text,
                Is.EqualTo("재접속 테스트"));

            ContentSizeFitter fitter = panel.GetComponent<ContentSizeFitter>();
            Assert.That(fitter, Is.Not.Null);
            Assert.That(
                fitter.horizontalFit,
                Is.EqualTo(ContentSizeFitter.FitMode.PreferredSize));

            var rect = (RectTransform)panel;
            LayoutRebuilder.ForceRebuildLayoutImmediate(rect);
            Assert.That(
                LayoutUtility.GetPreferredWidth(rect),
                Is.LessThanOrEqualTo(rect.rect.width),
                "F8 안내는 Arena 조작 패널을 넘치면 안 된다");
        }

        [Test]
        public void ArenaHudKeepsServerResultCodeChipOffThePlayerScreen()
        {
            GameObject hud = LoadPrefab(
                "Assets/Presentation/Prefabs/Ugui/ArenaHudPanel.prefab");
            Transform panel = hud.GetComponentsInChildren<Transform>(true)
                .Single(transform => transform.name == "ArenaTerminalPanel");

            // 이 칩은 서버 result code 를 그대로 비추는 개발용 표시다. 값이 공격 10종 +
            // 획득 12종의 protocol 코드이고 플레이어가 취할 동작이 없어서 화면에 두지 않는다.
            // 되살리면 촬영 내내 우상단에 Attack: Cooldown / Attack: OutOfRange 같은 코드가
            // 깜빡인다. 플레이어용 피드백은 ArenaTransientStatusChip 이 한국어로 담당한다.
            Assert.That(panel.gameObject.activeSelf, Is.False,
                "서버 result code 칩은 플레이어 화면에 노출하지 않는다");

            // 라벨 자체는 남겨 둔다. 에디터에서 패널만 켜면 다시 볼 수 있어야 하고,
            // 서버 authority 단정이 이 오브젝트 이름에 의존한다.
            Assert.That(
                panel.GetComponentsInChildren<Text>(true)
                    .Select(label => label.name),
                Is.EqualTo(new[] { "TerminalCopyLabel" }));
        }

        [Test]
        public void ArenaHudUsesSelectedCornerAnchorLayout()
        {
            GameObject hud = LoadPrefab(
                "Assets/Presentation/Prefabs/Ugui/ArenaHudPanel.prefab");
            RectTransform loot = hud.GetComponentsInChildren<RectTransform>(true)
                .Single(transform => transform.name == "ArenaLootChip");
            RectTransform transient = hud.GetComponentsInChildren<RectTransform>(true)
                .Single(transform => transform.name == "ArenaTransientStatusChip");

            Assert.That(loot.anchorMin, Is.EqualTo(new Vector2(1f, 0f)));
            Assert.That(loot.anchorMax, Is.EqualTo(new Vector2(1f, 0f)));
            Assert.That(loot.pivot, Is.EqualTo(new Vector2(1f, 0f)));
            Assert.That(loot.anchoredPosition, Is.EqualTo(new Vector2(-24f, 24f)));

            Assert.That(transient.anchorMin, Is.EqualTo(new Vector2(1f, 0f)));
            Assert.That(transient.anchorMax, Is.EqualTo(new Vector2(1f, 0f)));
            Assert.That(transient.pivot, Is.EqualTo(new Vector2(1f, 0f)));
            Assert.That(transient.anchoredPosition,
                Is.EqualTo(new Vector2(-24f, 96f)));
        }

        [Test]
        public void ArenaMonsterHudCopyStaysInsideThePanelTopPadding()
        {
            GameObject instance = Object.Instantiate(LoadPrefab(
                "Assets/Presentation/Prefabs/Ugui/ArenaHudPanel.prefab"));
            try
            {
                RectTransform panel = instance.GetComponentsInChildren<RectTransform>(true)
                    .Single(transform => transform.name == "ArenaMonsterHealthPanel");
                RectTransform label = instance.GetComponentsInChildren<RectTransform>(true)
                    .Single(transform => transform.name == "MonsterHitPointsLabel");
                Canvas.ForceUpdateCanvases();

                var panelCorners = new Vector3[4];
                var labelCorners = new Vector3[4];
                panel.GetWorldCorners(panelCorners);
                label.GetWorldCorners(labelCorners);
                Assert.That(panelCorners[1].y - labelCorners[1].y,
                    Is.GreaterThanOrEqualTo(16f));
            }
            finally
            {
                Object.DestroyImmediate(instance);
            }
        }

        [Test]
        public void ArenaDropUsesDistinctVendorSpritesForCommonAndRareItems()
        {
            GameObject instance = Object.Instantiate(LoadPrefab(
                "Assets/Presentation/Prefabs/World/ArenaDrop.prefab"));
            try
            {
                ArenaDropVisual visual = instance.GetComponent<ArenaDropVisual>();
                SpriteRenderer renderer = instance.GetComponent<SpriteRenderer>();
                Assert.That(visual, Is.Not.Null);

                // 컬렉션 행과 같은 두 파일이어야 한다. 아레나에서 주운 것과 목록에 쌓인 것이
                // 다르게 보이면 플레이어가 같은 아이템으로 인식하지 못한다.
                visual.Project(1);
                Sprite common = renderer.sprite;
                Assert.That(AssetDatabase.GetAssetPath(common),
                    Is.EqualTo(PresentationPackRoot + "content/item/relic_common.png"));
                visual.Project(2);
                Sprite rare = renderer.sprite;
                Assert.That(AssetDatabase.GetAssetPath(rare),
                    Is.EqualTo(PresentationPackRoot + "content/item/relic_rare.png"));

                // 색만으로 등급을 구분하지 않는다. 실루엣이 달라야 16픽셀에서도 읽힌다.
                Assert.That(common.rect.size, Is.EqualTo(rare.rect.size));
                Assert.That(
                    SilhouetteOf(PresentationPackRoot + "content/item/relic_common.png"),
                    Is.Not.EqualTo(
                        SilhouetteOf(PresentationPackRoot + "content/item/relic_rare.png")));

                foreach (string path in new[]
                         {
                             PresentationPackRoot + "content/item/relic_common.png",
                             PresentationPackRoot + "content/item/relic_rare.png"
                         })
                {
                    var importer = (TextureImporter)AssetImporter.GetAtPath(path);
                    Assert.That(importer.spritePixelsPerUnit, Is.EqualTo(16f), path);
                    Assert.That(importer.filterMode, Is.EqualTo(FilterMode.Point), path);
                    Assert.That(importer.mipmapEnabled, Is.False, path);
                    Assert.That(importer.textureCompression,
                        Is.EqualTo(TextureImporterCompression.Uncompressed), path);
                }
            }
            finally
            {
                Object.DestroyImmediate(instance);
            }
        }

        /// <summary>
        /// Opaque pixel mask of a PNG, as a string so two sprites can be compared for shape alone.
        /// A recolour of one gem would produce an identical mask and fail the caller. The file is
        /// decoded rather than sampled through the imported Texture2D because making a shipping
        /// texture CPU readable just to let a test look at it would cost memory at runtime.
        /// </summary>
        private static string SilhouetteOf(string assetPath)
        {
            string absolute = Path.Combine(
                Directory.GetParent(Application.dataPath).FullName,
                assetPath);
            var decoded = new Texture2D(1, 1);
            try
            {
                Assert.That(
                    decoded.LoadImage(File.ReadAllBytes(absolute), false),
                    Is.True,
                    assetPath);
                return string.Concat(decoded.GetPixels()
                    .Select(pixel => pixel.a > 0.5f ? '#' : '.'));
            }
            finally
            {
                Object.DestroyImmediate(decoded);
            }
        }

        [TestCase(1920, 1080)]
        [TestCase(1280, 720)]
        public void ArenaBackdropCoversFixedCameraViewport(int width, int height)
        {
            Scene scene = EditorSceneManager.OpenScene(
                "Assets/Scenes/ArenaScene.unity",
                OpenSceneMode.Additive);
            try
            {
                Camera camera = scene.GetRootGameObjects()
                    .SelectMany(root => root.GetComponentsInChildren<Camera>(true))
                    .Single();
                float halfHeight = camera.orthographicSize;
                float halfWidth = halfHeight * width / height;
                Assert.That(halfHeight, Is.GreaterThanOrEqualTo(10f));
                Assert.That(halfWidth, Is.GreaterThanOrEqualTo(10f));

                GameObject backdrop = LoadPrefab(
                    "Assets/Presentation/Prefabs/World/ArenaBackdrop.prefab");
                Grid grid = backdrop.GetComponent<Grid>();
                Assert.That(grid.cellSize, Is.EqualTo(Vector3.one));
                Tilemap ground = backdrop.GetComponentsInChildren<Tilemap>(true)
                    .Single(tilemap => tilemap.name == "Ground");
                BoundsInt cells = ground.cellBounds;
                Assert.That(cells.xMin,
                    Is.LessThanOrEqualTo(Mathf.FloorToInt(-halfWidth)));
                Assert.That(cells.xMax,
                    Is.GreaterThanOrEqualTo(Mathf.CeilToInt(halfWidth)));
                Assert.That(cells.yMin,
                    Is.LessThanOrEqualTo(Mathf.FloorToInt(-halfHeight)));
                Assert.That(cells.yMax,
                    Is.GreaterThanOrEqualTo(Mathf.CeilToInt(halfHeight)));
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void ArenaBackdropKeepsSideColumnsAsGrassOnly()
        {
            GameObject backdrop = LoadPrefab(
                "Assets/Presentation/Prefabs/World/ArenaBackdrop.prefab");
            Transform props = backdrop.transform.Find("Props");

            Assert.That(props, Is.Not.Null);
            Assert.That(props.childCount, Is.Zero);
        }

        private static GameObject LoadPrefab(string path)
        {
            GameObject prefab = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            Assert.That(prefab, Is.Not.Null, path);
            return prefab;
        }

        private static void AssertAssetPath(Object asset, string relative)
        {
            Assert.That(asset, Is.Not.Null, relative);
            Assert.That(
                AssetDatabase.GetAssetPath(asset),
                Is.EqualTo(KenneyRoot + relative));
        }

        private static void AssertUniformScale(string path, float expected)
        {
            Vector3 scale = LoadPrefab(path).transform.localScale;
            Assert.That(scale.x, Is.EqualTo(expected).Within(0.001f), path);
            Assert.That(scale.y, Is.EqualTo(expected).Within(0.001f), path);
            Assert.That(scale.z, Is.EqualTo(expected).Within(0.001f), path);
        }

        private static void AssertSpriteImporter(string path, Vector4 border)
        {
            var importer = (TextureImporter)AssetImporter.GetAtPath(path);
            Assert.That(importer, Is.Not.Null, path);
            Assert.That(importer.textureType, Is.EqualTo(TextureImporterType.Sprite), path);
            Assert.That(importer.spriteImportMode, Is.EqualTo(SpriteImportMode.Single), path);
            Assert.That(importer.spritePixelsPerUnit, Is.EqualTo(32f), path);
            Assert.That(importer.spriteBorder, Is.EqualTo(border), path);
            Assert.That(importer.filterMode, Is.EqualTo(FilterMode.Point), path);
            Assert.That(importer.mipmapEnabled, Is.False, path);
            Assert.That(
                importer.textureCompression,
                Is.EqualTo(TextureImporterCompression.Uncompressed),
                path);
        }
    }
}
