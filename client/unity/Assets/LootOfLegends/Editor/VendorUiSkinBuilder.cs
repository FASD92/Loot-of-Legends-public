using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Presentation.Common;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.UI;
using Object = UnityEngine.Object;

namespace LootOfLegends.Editor
{
    /// <summary>
    /// Applies the vendor UI art that plain text and untextured quads were standing in for.
    ///
    /// Three screens change. The Arena control hint stops spelling key names in a sentence and
    /// shows the actual keyboard caps next to each verb. The monster health fill stops being a
    /// tinted white quad and becomes the pack's life bar, whose one pixel end caps need a
    /// horizontal three slice border to survive stretching. The relic icons stop being two hand
    /// drawn placeholders and become two gems that differ in both colour and silhouette, so
    /// rarity reads without relying on colour alone.
    ///
    /// This runs as a builder rather than as hand edited YAML because the work spans importer
    /// settings, a ScriptableObject entry list, prefab hierarchy, and a Scene reference, and
    /// because every new GameObject is looked up by name before being created so a rerun keeps
    /// file ids stable. A regenerated file id would silently break the Scene bindings.
    /// </summary>
    public static class VendorUiSkinBuilder
    {
        private const string PackRoot = "Assets/ThirdParty/PresentationPack/";
        private const string CatalogPath = "Assets/Presentation/PresentationCatalog.asset";
        private const string HudPrefab = "Assets/Presentation/Prefabs/Ugui/ArenaHudPanel.prefab";
        private const string DropPrefab = "Assets/Presentation/Prefabs/World/ArenaDrop.prefab";
        private const string ArenaScenePath = "Assets/Scenes/ArenaScene.unity";

        private const string HintPanelName = "ArenaInputHintPanel";
        private const string HealthFillName = "MonsterHealthFill";

        private const string LegacyCommonRelic =
            "Assets/Presentation/Prefabs/World/CommonRelic.png";
        private const string LegacyRareRelic =
            "Assets/Presentation/Prefabs/World/RareRelic.png";

        // Every vendor sprite in this pack is authored at 16 pixels per unit, which is what the
        // existing character sheets and map atlases already use.
        private const float PixelsPerUnit = 16f;

        // The caps are 13 pixels tall and the hint panel is 58 tall, so a whole number scale of
        // two lands at 26 and leaves room for the panel border. A fractional scale would resample
        // pixel art and undo the reason for using it.
        private const float CapScale = 2f;

        private const int HintFontSize = 26;
        private const float HintSpacing = 6f;

        // Wider than the uniform spacing so each cap reads as belonging to the verb after it
        // rather than to the verb before it.
        private const float HintGroupGap = 16f;

        // A sliced border is converted to canvas units by dividing the sprite pixel border by
        // (sprite pixelsPerUnit / Canvas.referencePixelsPerUnit) * Image.pixelsPerUnitMultiplier.
        // This pack authors at 16 against a Canvas reference of 100, so at the default multiplier
        // the bar's one pixel outline becomes 6.25 units per edge and all but a hairline of the
        // 14 unit tall fill area is outline. 3.125 divides that back down to a two unit outline
        // and leaves ten units of red core.
        private const float LifeBarPixelsPerUnitMultiplier = 3.125f;

        private static readonly (string Key, string Path, Vector4 Border)[] Sprites =
        {
            ("ui.key.w", "content/ui/input/key_w.png", default),
            ("ui.key.a", "content/ui/input/key_a.png", default),
            ("ui.key.s", "content/ui/input/key_s.png", default),
            ("ui.key.d", "content/ui/input/key_d.png", default),
            ("ui.key.space", "content/ui/input/key_space.png", default),
            ("ui.key.e", "content/ui/input/key_e.png", default),
            // The bar is a one pixel dark outline around a two pixel red core, so only that
            // outline is fixed and everything inside it stretches.
            ("ui.lifebar.progress", "content/ui/life_bar_progress.png", new Vector4(1f, 1f, 1f, 1f)),
            ("loot.relic.common", "content/item/relic_common.png", default),
            ("loot.relic.rare", "content/item/relic_rare.png", default)
        };

        public static void Build()
        {
            foreach ((string _, string relative, Vector4 border) in Sprites)
            {
                ImportSprite(PackRoot + relative, border);
            }

            Dictionary<string, Sprite> resolved = Sprites.ToDictionary(
                entry => entry.Key,
                entry => Load(PackRoot + entry.Path));

            WriteCatalog(resolved);
            RebuildHud(resolved);
            NormaliseEmptyValues(HudPrefab);
            RebindArenaScene();
            RepointDrop(resolved);
            NormaliseEmptyValues(DropPrefab);
            RetireLegacyRelics();

            AssetDatabase.SaveAssets();
            Debug.Log("VendorUiSkinBuilder completed");
        }

        /// <summary>
        /// Applies the shared importer contract. Every setting is written unconditionally: when
        /// nothing actually changes Unity skips writing the meta file, and a border assigned in
        /// that state is silently dropped.
        /// </summary>
        private static void ImportSprite(string path, Vector4 border)
        {
            var importer = AssetImporter.GetAtPath(path) as TextureImporter;
            if (importer == null)
            {
                throw new InvalidOperationException($"Missing texture importer for {path}");
            }

            importer.textureType = TextureImporterType.Sprite;
            importer.spriteImportMode = SpriteImportMode.Single;
            importer.spritePixelsPerUnit = PixelsPerUnit;
            importer.spriteBorder = border;
            importer.filterMode = FilterMode.Point;
            importer.mipmapEnabled = false;
            importer.alphaIsTransparency = true;
            importer.textureCompression = TextureImporterCompression.Uncompressed;
            EditorUtility.SetDirty(importer);
            importer.SaveAndReimport();
            AssetDatabase.ImportAsset(
                path,
                ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport);
            Debug.Log($"VendorUiSkinBuilder imported={path} border={border}");
        }

        private static Sprite Load(string path)
        {
            Sprite sprite = AssetDatabase.LoadAssetAtPath<Sprite>(path);
            if (sprite == null)
            {
                throw new InvalidOperationException($"Missing sprite at {path}");
            }
            return sprite;
        }

        private static void WriteCatalog(IReadOnlyDictionary<string, Sprite> sprites)
        {
            PresentationCatalog catalog =
                AssetDatabase.LoadAssetAtPath<PresentationCatalog>(CatalogPath);
            if (catalog == null)
            {
                throw new InvalidOperationException($"Missing catalog at {CatalogPath}");
            }

            var serialized = new SerializedObject(catalog);
            SerializedProperty entries = serialized.FindProperty("entries");
            int added = 0;
            foreach ((string key, Sprite sprite) in sprites.OrderBy(entry => entry.Key))
            {
                SerializedProperty match = null;
                for (int index = 0; index < entries.arraySize; index++)
                {
                    SerializedProperty candidate = entries.GetArrayElementAtIndex(index);
                    if (candidate.FindPropertyRelative("key").stringValue == key)
                    {
                        match = candidate;
                        break;
                    }
                }
                if (match == null)
                {
                    entries.arraySize++;
                    match = entries.GetArrayElementAtIndex(entries.arraySize - 1);
                    match.FindPropertyRelative("key").stringValue = key;
                    added++;
                }
                match.FindPropertyRelative("asset").objectReferenceValue = sprite;
            }
            serialized.ApplyModifiedPropertiesWithoutUndo();
            EditorUtility.SetDirty(catalog);
            Debug.Log(
                $"VendorUiSkinBuilder catalog entries={entries.arraySize} added={added}");
        }

        private static void RebuildHud(IReadOnlyDictionary<string, Sprite> sprites)
        {
            GameObject contents = PrefabUtility.LoadPrefabContents(HudPrefab);
            try
            {
                Transform panel = Find(contents, HintPanelName);
                Text existing = Find(contents, "InputHintLabel").GetComponent<Text>();
                if (existing == null)
                {
                    throw new InvalidOperationException("InputHintLabel must carry a Text");
                }

                var group = Ensure<HorizontalLayoutGroup>(panel.gameObject);
                group.childAlignment = TextAnchor.MiddleCenter;
                group.spacing = HintSpacing;
                group.padding = new RectOffset(12, 12, 4, 4);
                group.childControlWidth = true;
                group.childControlHeight = true;
                group.childForceExpandWidth = false;
                group.childForceExpandHeight = false;
                group.childScaleWidth = false;
                group.childScaleHeight = false;

                var fitter = Ensure<ContentSizeFitter>(panel.gameObject);
                fitter.horizontalFit = ContentSizeFitter.FitMode.PreferredSize;
                fitter.verticalFit = ContentSizeFitter.FitMode.Unconstrained;

                // Left to right reading order. The pre-existing InputHintLabel is reused in place
                // so the Scene binding that points at it keeps resolving.
                var order = new List<Transform>
                {
                    KeyCap(panel, "KeyCapW", sprites["ui.key.w"]),
                    KeyCap(panel, "KeyCapA", sprites["ui.key.a"]),
                    KeyCap(panel, "KeyCapS", sprites["ui.key.s"]),
                    KeyCap(panel, "KeyCapD", sprites["ui.key.d"]),
                    Style(existing, HintCopy("MoveHint")).transform,
                    Gap(panel, "MoveHintGap"),
                    KeyCap(panel, "KeyCapSpace", sprites["ui.key.space"]),
                    Label(panel, "AttackHintLabel", existing, HintCopy("AttackHint")).transform,
                    Gap(panel, "AttackHintGap"),
                    KeyCap(panel, "KeyCapE", sprites["ui.key.e"]),
                    Label(panel, "ClaimHintLabel", existing, HintCopy("ClaimHint")).transform,
                    Gap(panel, "ReconnectHintGap"),
                    Label(panel, "KeyCapF8", existing, "[F8]").transform,
                    Label(panel, "ReconnectHintLabel", existing, "재접속 테스트").transform
                };
                for (int index = 0; index < order.Count; index++)
                {
                    order[index].SetSiblingIndex(index);
                }

                Image fill = Find(contents, HealthFillName).GetComponent<Image>();
                if (fill == null)
                {
                    throw new InvalidOperationException($"{HealthFillName} must carry an Image");
                }
                fill.sprite = sprites["ui.lifebar.progress"];
                fill.type = Image.Type.Sliced;
                fill.pixelsPerUnitMultiplier = LifeBarPixelsPerUnitMultiplier;
                // The old quad carried the bar colour as a tint. The sprite brings its own colour,
                // and leaving the dark tint in place would cancel out the reason for the swap.
                fill.color = Color.white;

                PrefabUtility.SaveAsPrefabAsset(contents, HudPrefab);
                Debug.Log(
                    $"VendorUiSkinBuilder hud hintChildren={panel.childCount} " +
                    $"fill={fill.sprite.name} fillType={fill.type}");
            }
            finally
            {
                PrefabUtility.UnloadPrefabContents(contents);
            }
        }

        private static Transform KeyCap(Transform parent, string name, Sprite sprite)
        {
            GameObject cap = GetOrCreate(parent, name);
            var image = Ensure<Image>(cap);
            image.sprite = sprite;
            image.type = Image.Type.Simple;
            image.color = Color.white;
            image.raycastTarget = false;
            image.preserveAspect = true;

            var element = Ensure<LayoutElement>(cap);
            element.preferredWidth = sprite.rect.width * CapScale;
            element.preferredHeight = sprite.rect.height * CapScale;
            return cap.transform;
        }

        private static Transform Gap(Transform parent, string name)
        {
            GameObject gap = GetOrCreate(parent, name);
            var element = Ensure<LayoutElement>(gap);
            element.preferredWidth = HintGroupGap;
            element.preferredHeight = 1f;
            return gap.transform;
        }

        private static Text Label(Transform parent, string name, Text template, string copy)
        {
            GameObject owner = GetOrCreate(parent, name);
            var label = Ensure<Text>(owner);
            label.font = template.font;
            label.color = template.color;
            label.raycastTarget = false;
            return Style(label, copy);
        }

        private static Text Style(Text label, string copy)
        {
            label.fontSize = HintFontSize;
            label.alignment = TextAnchor.MiddleCenter;
            // A layout group sizes each label to its unwrapped width, so wrapping would only ever
            // fire on a rounding error and clip a verb to a single character.
            label.horizontalOverflow = HorizontalWrapMode.Overflow;
            label.verticalOverflow = VerticalWrapMode.Overflow;
            // The authored text has to match what the View writes at runtime, otherwise the Scene
            // asset test and the PlayMode test disagree about the same label.
            label.text = copy;
            return label;
        }

        /// <summary>
        /// Reads the hint wording straight off ArenaScreenView so the authored asset cannot drift
        /// from the code that overwrites it every frame.
        /// </summary>
        private static string HintCopy(string constantName)
        {
            FieldInfo field = typeof(ArenaScreenView).GetField(
                constantName,
                BindingFlags.NonPublic | BindingFlags.Static);
            if (field == null || !field.IsLiteral)
            {
                throw new InvalidOperationException(
                    $"ArenaScreenView is missing the {constantName} constant");
            }
            return (string)field.GetRawConstantValue();
        }

        private static GameObject GetOrCreate(Transform parent, string name)
        {
            Transform existing = parent.Cast<Transform>()
                .FirstOrDefault(child => child.name == name);
            if (existing != null)
            {
                return existing.gameObject;
            }
            var created = new GameObject(name, typeof(RectTransform));
            created.transform.SetParent(parent, false);
            return created;
        }

        private static T Ensure<T>(GameObject owner) where T : Component
        {
            return owner.GetComponent<T>() ?? owner.AddComponent<T>();
        }

        /// <summary>
        /// The two added hint labels live in the prefab, but the ArenaScreenView that drives them
        /// lives in the Scene, so the Scene is where the references have to be written.
        /// </summary>
        private static void RebindArenaScene()
        {
            Scene scene = EditorSceneManager.OpenScene(ArenaScenePath, OpenSceneMode.Single);
            ArenaScreenView view = scene.GetRootGameObjects()
                .SelectMany(root => root.GetComponentsInChildren<ArenaScreenView>(true))
                .Single();
            var serialized = new SerializedObject(view);
            foreach (string field in new[] { "attackHintLabel", "claimHintLabel" })
            {
                SerializedProperty property = serialized.FindProperty(field);
                if (property == null)
                {
                    throw new InvalidOperationException(
                        $"ArenaScreenView is missing the {field} field");
                }
                string labelName = char.ToUpperInvariant(field[0]) + field.Substring(1);
                property.objectReferenceValue = view
                    .GetComponentsInChildren<Text>(true)
                    .Single(label => label.name == labelName);
            }
            serialized.ApplyModifiedPropertiesWithoutUndo();
            EditorSceneManager.MarkSceneDirty(scene);
            EditorSceneManager.SaveScene(scene);
            NormaliseEmptyValues(ArenaScenePath);
            Debug.Log("VendorUiSkinBuilder rebound ArenaScene hint labels");
        }

        /// <summary>
        /// Rewrites "key: " as "key:" for empty values. Saving a Scene makes this serializer emit a
        /// trailing space that the other three Scenes in the repository do not have, so leaving it
        /// would bury the two changed references under twenty lines of whitespace churn and make
        /// this one Scene inconsistent with its siblings. Both forms parse as the empty string, and
        /// doing it here rather than by hand keeps a rerun of the builder byte stable.
        /// </summary>
        private static void NormaliseEmptyValues(string assetPath)
        {
            string absolute = Path.Combine(
                Directory.GetParent(Application.dataPath).FullName,
                assetPath);
            string[] lines = File.ReadAllLines(absolute);
            int trimmed = 0;
            for (int index = 0; index < lines.Length; index++)
            {
                if (lines[index].EndsWith(": ", StringComparison.Ordinal))
                {
                    lines[index] = lines[index].TrimEnd();
                    trimmed++;
                }
            }
            if (trimmed == 0)
            {
                return;
            }
            File.WriteAllText(absolute, string.Join("\n", lines) + "\n");
            AssetDatabase.ImportAsset(
                assetPath,
                ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport);
            Debug.Log($"VendorUiSkinBuilder normalised={assetPath} emptyValues={trimmed}");
        }

        private static void RepointDrop(IReadOnlyDictionary<string, Sprite> sprites)
        {
            GameObject contents = PrefabUtility.LoadPrefabContents(DropPrefab);
            try
            {
                ArenaDropVisual visual = contents.GetComponent<ArenaDropVisual>();
                if (visual == null)
                {
                    throw new InvalidOperationException("ArenaDrop must carry ArenaDropVisual");
                }
                var serialized = new SerializedObject(visual);
                serialized.FindProperty("commonSprite").objectReferenceValue =
                    sprites["loot.relic.common"];
                serialized.FindProperty("rareSprite").objectReferenceValue =
                    sprites["loot.relic.rare"];
                serialized.ApplyModifiedPropertiesWithoutUndo();

                PrefabUtility.SaveAsPrefabAsset(contents, DropPrefab);
                Debug.Log("VendorUiSkinBuilder repointed ArenaDrop relic sprites");
            }
            finally
            {
                PrefabUtility.UnloadPrefabContents(contents);
            }
        }

        private static void RetireLegacyRelics()
        {
            foreach (string path in new[] { LegacyCommonRelic, LegacyRareRelic })
            {
                if (AssetDatabase.LoadAssetAtPath<Object>(path) == null)
                {
                    continue;
                }
                if (!AssetDatabase.DeleteAsset(path))
                {
                    throw new InvalidOperationException($"Could not delete {path}");
                }
                Debug.Log($"VendorUiSkinBuilder retired={path}");
            }
        }

        private static Transform Find(GameObject contents, string name)
        {
            Transform match = contents.GetComponentsInChildren<Transform>(true)
                .SingleOrDefault(transform => transform.name == name);
            if (match == null)
            {
                throw new InvalidOperationException($"{contents.name} has no '{name}'");
            }
            return match;
        }
    }
}
