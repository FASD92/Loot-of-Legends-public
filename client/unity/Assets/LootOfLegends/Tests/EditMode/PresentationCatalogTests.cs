using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Presentation.Common;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using Object = UnityEngine.Object;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class PresentationCatalogTests
    {
        private const string PresentationPackRoot =
            "Assets/ThirdParty/PresentationPack/";
        private const string KenneyRoot =
            "Assets/ThirdParty/KenneyUIPixelAdventure/";
        private const string KenneyAudioRoot =
            "Assets/ThirdParty/KenneyAudio/";

        private static readonly (string Key, string Path)[] KenneyUiMappings =
        {
            ("ui.button.disabled", "large-thin/tile_0000.png"),
            ("ui.button.hover", "large-thin/tile_0001.png"),
            ("ui.button.normal", "large-thick/tile_0001.png"),
            ("ui.button.pressed", "large-thick/tile_0014.png"),
            ("ui.background", "large-thin/tile_0000.png"),
            ("ui.panel", "large-thick/tile_0000.png")
        };

        private static readonly (string Key, string Path)[] SfxMappings =
        {
            ("audio.sfx.ui-click", "ui/click3.ogg"),
            ("audio.sfx.attack", "rpg/knifeSlice.ogg"),
            ("audio.sfx.hit", "impact/impactPunch_medium_002.ogg"),
            ("audio.sfx.monster-death", "impact/impactSoft_heavy_000.ogg"),
            ("audio.sfx.loot-pickup", "rpg/handleCoins2.ogg"),
            ("audio.sfx.result", "ui/switch31.ogg")
        };

        private static readonly string[] ArenaPlayerSheetHashes =
        {
            "f2dd61a264c251b81e63da7a28ab0fdccd261b807e5fa7d1832a468e14a21078",
            "83d98ce273ec2c022c331d3d1ce8c77657c29a55b69951a532b749ac3c9f9b22",
            "add7add88a9f391e776ede8d0ef9246850bedf22b3510f4c8366886df8101a25",
            "8e45c30adcd8a0d7f2f384e28dd5bfd54bbcca096ac3d05d68d4afd0cd8eb186",
            "a0b353001f494207b868a22479a2689e4de044af59ffa5b79f1a840a973d486f",
            "c99c8142f4813562a4e76e96d93bbe8e0b04c22e0625c98b75cf564c99e36358",
            "805dd2d4dacc46ed5fbad6b786da92dd61d63f9678cf10855c197cb52dd68fcb",
            "447bd8fec385066b3b358671932d979b6e420876e4fd5daf04a038c1b6887209",
            "c7e3b2087b3143e3415e3c1d078792186586a58d9f002531018febf2ba4ab29e",
            "ce464d2f306c0eacd18380a48352d5018bc9a82ab46fff6fe17feef41c0236c8"
        };

        private static readonly string[] ArenaPlayerSheetPaths =
        {
            "content/character/ninja_blue/sprite.png",
            "content/character/ninja_bomb/sprite.png",
            "content/character/ninja_dark/sprite.png",
            "content/character/ninja_eskimo/sprite.png",
            "content/character/ninja_fire/sprite.png",
            "content/character/ninja_green/sprite.png",
            "content/character/ninja_leaf/sprite.png",
            "content/character/ninja_red/sprite.png",
            "content/character/ninja_thunder/sprite.png",
            "content/character/ninja_water/sprite.png"
        };

        private static readonly string[] RequiredKeys =
        {
            "audio.music.theme",
            "audio.sfx.ui-click",
            "audio.sfx.attack",
            "audio.sfx.hit",
            "audio.sfx.monster-death",
            "audio.sfx.loot-pickup",
            "audio.sfx.result",
            "character.shadow",
            "character.ninja.blue",
            "character.pig",
            "character.samurai.blue",
            "character.samurai.green",
            "destroyable.crate",
            "map.animated",
            "map.floor",
            "map.village",
            "map.wall",
            "particle.leaf",
            "particle.rock",
            "particle.wood",
            "ui.heart.full",
            "ui.heart.variant2",
            "ui.heart.variant3",
            "ui.key.w",
            "ui.key.a",
            "ui.key.s",
            "ui.key.d",
            "ui.key.space",
            "ui.key.e",
            "ui.lifebar.progress",
            "weapon.sword",
            "weapon.club",
            "font.normal",
            "ui.button.disabled",
            "ui.button.hover",
            "ui.button.normal",
            "ui.button.pressed",
            "ui.background",
            "ui.panel",
            "ui.checked",
            "ui.unchecked"
        };

        [Test]
        public void ResolveReturnsTheMappedAsset()
        {
            PresentationCatalog catalog =
                ScriptableObject.CreateInstance<PresentationCatalog>();
            var texture = new Texture2D(1, 1);
            try
            {
                Configure(catalog, ("ui.panel", texture));

                Assert.That(catalog.Resolve<Texture2D>("ui.panel"), Is.SameAs(texture));
            }
            finally
            {
                Object.DestroyImmediate(texture);
                Object.DestroyImmediate(catalog);
            }
        }

        [Test]
        public void ArenaPlayerAppearanceTypesAndCatalogEntryExist()
        {
            Type appearanceType = typeof(PresentationCatalog).Assembly.GetType(
                "LootOfLegends.Presentation.Arena.ArenaPlayerAppearanceSet");
            Type facingType = typeof(PresentationCatalog).Assembly.GetType(
                "LootOfLegends.Presentation.Arena.ArenaFacing");
            Type visualType = typeof(PresentationCatalog).Assembly.GetType(
                "LootOfLegends.Presentation.Arena.ArenaPlayerVisual");
            Assert.That(appearanceType, Is.Not.Null);
            Assert.That(facingType, Is.Not.Null);
            Assert.That(visualType, Is.Not.Null);
            Assert.That(
                appearanceType.GetMethod(
                    "Resolve",
                    new[] { typeof(int), facingType, typeof(int) }),
                Is.Not.Null);
            Assert.That(
                visualType.GetMethod("Bind", new[] { appearanceType, typeof(int) }),
                Is.Not.Null);
            Assert.That(
                visualType.GetMethod("Project", new[] { typeof(Vector3) }),
                Is.Not.Null);

            PresentationCatalog catalog = AssetDatabase.LoadAssetAtPath<PresentationCatalog>(
                "Assets/Presentation/PresentationCatalog.asset");
            Assert.DoesNotThrow(() =>
                catalog.Resolve<Object>("character.player.appearance-set"));
        }

        [Test]
        public void CollectionRelicKeysResolveTheVendorGemSprites()
        {
            PresentationCatalog catalog = AssetDatabase.LoadAssetAtPath<PresentationCatalog>(
                "Assets/Presentation/PresentationCatalog.asset");

            // 아레나 드롭도 같은 두 파일을 쓴다(UguiPresentationAssetTests). 두 화면이 갈리면
            // 주운 아이템과 목록의 아이템이 같은 것으로 안 보인다.
            Assert.That(
                AssetDatabase.GetAssetPath(catalog.Resolve<Sprite>("loot.relic.common")),
                Is.EqualTo(PresentationPackRoot + "content/item/relic_common.png"));
            Assert.That(
                AssetDatabase.GetAssetPath(catalog.Resolve<Sprite>("loot.relic.rare")),
                Is.EqualTo(PresentationPackRoot + "content/item/relic_rare.png"));

            // 프로젝트가 직접 그린 placeholder 는 물러났다. 남겨두면 어느 쪽이 쓰이는지
            // 알 수 없는 상태로 두 벌이 공존한다.
            foreach (string retired in new[]
                     {
                         "Assets/Presentation/Prefabs/World/CommonRelic.png",
                         "Assets/Presentation/Prefabs/World/RareRelic.png"
                     })
            {
                Assert.That(
                    AssetDatabase.LoadAssetAtPath<Sprite>(retired),
                    Is.Null,
                    retired);
            }
        }

        [Test]
        public void ArenaAppearanceSetContainsTenUniqueFourDirectionAnimations()
        {
            PresentationCatalog catalog = AssetDatabase.LoadAssetAtPath<PresentationCatalog>(
                "Assets/Presentation/PresentationCatalog.asset");
            ArenaPlayerAppearanceSet appearances =
                catalog.Resolve<ArenaPlayerAppearanceSet>(
                    "character.player.appearance-set");
            var idleSprites = new HashSet<Sprite>();

            for (int slot = 0; slot < ArenaPlayerAppearanceSet.AppearanceCount; slot++)
            {
                foreach (ArenaFacing facing in Enum.GetValues(typeof(ArenaFacing)))
                {
                    for (int frame = 0; frame < 4; frame++)
                    {
                        Sprite sprite = appearances.Resolve(slot, facing, frame);
                        Assert.That(sprite, Is.Not.Null, $"slot={slot} {facing} {frame}");
                        Assert.That(sprite.rect.size, Is.EqualTo(new Vector2(16f, 16f)));
                    }
                }
                Assert.That(idleSprites.Add(
                    appearances.Resolve(slot, ArenaFacing.Down, 0)),
                    Is.True,
                    "each player slot must use a distinct sheet");
            }

            Assert.Throws<ArgumentOutOfRangeException>(() =>
                appearances.Resolve(-1, ArenaFacing.Down, 0));
            Assert.Throws<ArgumentOutOfRangeException>(() =>
                appearances.Resolve(0, ArenaFacing.Down, 4));
        }

        [Test]
        public void ResolveRejectsDuplicateAndMissingKeys()
        {
            PresentationCatalog catalog =
                ScriptableObject.CreateInstance<PresentationCatalog>();
            var first = new Texture2D(1, 1);
            var second = new Texture2D(1, 1);
            try
            {
                Configure(catalog, ("ui.panel", first), ("ui.panel", second));
                Assert.Throws<InvalidOperationException>(
                    () => catalog.Resolve<Texture2D>("ui.panel"));

                Configure(catalog, ("ui.panel", first));
                Assert.Throws<KeyNotFoundException>(
                    () => catalog.Resolve<Texture2D>("ui.missing"));
            }
            finally
            {
                Object.DestroyImmediate(first);
                Object.DestroyImmediate(second);
                Object.DestroyImmediate(catalog);
            }
        }

        [Test]
        public void RepositoryCatalogAndProvenanceAreComplete()
        {
            const string catalogPath =
                "Assets/Presentation/PresentationCatalog.asset";
            PresentationCatalog catalog =
                AssetDatabase.LoadAssetAtPath<PresentationCatalog>(catalogPath);
            Assert.That(catalog, Is.Not.Null, catalogPath);

            string[] assetPaths = RequiredKeys
                .Select(key => AssetDatabase.GetAssetPath(catalog.Resolve<Object>(key)))
                .ToArray();
            Assert.That(
                assetPaths.All(path =>
                    path.StartsWith(PresentationPackRoot, StringComparison.Ordinal) ||
                    path.StartsWith(KenneyRoot, StringComparison.Ordinal) ||
                    path.StartsWith(KenneyAudioRoot, StringComparison.Ordinal)),
                Is.True);
            Assert.That(assetPaths.Distinct().Count(), Is.EqualTo(RequiredKeys.Length - 1));
            foreach ((string key, string relative) in KenneyUiMappings)
            {
                Assert.That(
                    AssetDatabase.GetAssetPath(catalog.Resolve<Object>(key)),
                    Is.EqualTo(KenneyRoot + relative),
                    key);
            }
            foreach ((string key, string relative) in SfxMappings)
            {
                Object clip = catalog.Resolve<Object>(key);
                Assert.That(clip, Is.TypeOf<AudioClip>(), key);
                Assert.That(
                    AssetDatabase.GetAssetPath(clip),
                    Is.EqualTo(KenneyAudioRoot + relative),
                    key);
            }

            string presentationPackRoot = Path.Combine(
                Directory.GetParent(Application.dataPath).FullName,
                PresentationPackRoot);
            string license = File.ReadAllText(
                Path.Combine(presentationPackRoot, "LICENSE.md"));
            StringAssert.Contains(
                "6ac78232d5aedcc85ce5f27d060ea92366f7c24a",
                license);
            StringAssert.Contains("Upload ID: `16981275`", license);
            StringAssert.Contains(
                "95a06f4fdcfd1882f061a45ff313b7c905dbe2de1e8512b281d7937df62a7b15",
                license);
            StringAssert.Contains("CC0 1.0", license);

            string[] inventory = File.ReadAllLines(
                    Path.Combine(presentationPackRoot, "INVENTORY.sha256"))
                .Where(line => !string.IsNullOrWhiteSpace(line))
                .ToArray();
            Assert.That(inventory, Has.Length.EqualTo(46));
            foreach (string playerSheetHash in ArenaPlayerSheetHashes)
            {
                Assert.That(inventory, Has.Some.StartsWith(playerSheetHash + "  "));
                StringAssert.Contains(playerSheetHash, license);
            }
            foreach (string line in inventory)
            {
                int separator = line.IndexOf("  ", StringComparison.Ordinal);
                Assert.That(separator, Is.EqualTo(64), line);
                string expected = line.Substring(0, separator);
                string relative = line.Substring(separator + 2);
                string actual = Sha256(Path.Combine(presentationPackRoot, relative));
                Assert.That(actual, Is.EqualTo(expected), relative);
            }

            string kenneyRoot = Path.Combine(
                Directory.GetParent(Application.dataPath).FullName,
                KenneyRoot);
            string kenneyLicense = File.ReadAllText(
                Path.Combine(kenneyRoot, "LICENSE.md"));
            StringAssert.Contains("UI Pack - Pixel Adventure (2.0)", kenneyLicense);
            StringAssert.Contains("kenney_ui-pack-pixel-adventure.zip", kenneyLicense);
            StringAssert.Contains(
                "6ebf462e7f209f5f348419b09be6601a559ef1e1d6b595f0e9f8aa4c00a84048",
                kenneyLicense);
            StringAssert.Contains("CC0", kenneyLicense);

            string[] kenneyInventory = File.ReadAllLines(
                    Path.Combine(kenneyRoot, "INVENTORY.sha256"))
                .Where(line => !string.IsNullOrWhiteSpace(line))
                .ToArray();
            Assert.That(kenneyInventory, Has.Length.EqualTo(6));
            foreach (string line in kenneyInventory)
            {
                int separator = line.IndexOf("  ", StringComparison.Ordinal);
                Assert.That(separator, Is.EqualTo(64), line);
                string expected = line.Substring(0, separator);
                string relative = line.Substring(separator + 2);
                string actual = Sha256(Path.Combine(kenneyRoot, relative));
                Assert.That(actual, Is.EqualTo(expected), relative);
            }

            string kenneyAudioRoot = Path.Combine(
                Directory.GetParent(Application.dataPath).FullName,
                KenneyAudioRoot);
            string audioLicense = File.ReadAllText(
                Path.Combine(kenneyAudioRoot, "LICENSE.md"));
            StringAssert.Contains("CC0", audioLicense);
            StringAssert.Contains(
                "946fc23a63d535d693eb31b2eabb80c8c28d6351e2186b344ceb71b2cb1d5eb6",
                audioLicense);
            StringAssert.Contains(
                "6dbeaf8544da958d8f2adcb4a4a4b76c1ade34a05f8ab9edccd327da7375f38b",
                audioLicense);
            StringAssert.Contains(
                "029d734af1582474edf3a694d1b0cebc97c1c152f2f39fa34d4c2bafc5de77f8",
                audioLicense);

            string[] audioInventory = File.ReadAllLines(
                    Path.Combine(kenneyAudioRoot, "INVENTORY.sha256"))
                .Where(line => !string.IsNullOrWhiteSpace(line))
                .ToArray();
            Assert.That(audioInventory, Has.Length.EqualTo(6));
            foreach (string line in audioInventory)
            {
                int separator = line.IndexOf("  ", StringComparison.Ordinal);
                Assert.That(separator, Is.EqualTo(64), line);
                string expected = line.Substring(0, separator);
                string relative = line.Substring(separator + 2);
                string actual = Sha256(Path.Combine(kenneyAudioRoot, relative));
                Assert.That(actual, Is.EqualTo(expected), relative);
            }

            foreach (string relative in ArenaPlayerSheetPaths)
            {
                string assetPath = PresentationPackRoot + relative;
                Texture2D texture = AssetDatabase.LoadAssetAtPath<Texture2D>(assetPath);
                Assert.That(texture, Is.Not.Null, assetPath);
                Assert.That(texture.width, Is.EqualTo(64), assetPath);
                Assert.That(texture.height, Is.EqualTo(112), assetPath);

                var importer = (TextureImporter)AssetImporter.GetAtPath(assetPath);
                Assert.That(importer.spriteImportMode,
                    Is.EqualTo(SpriteImportMode.Multiple), assetPath);
                Assert.That(importer.spritePixelsPerUnit, Is.EqualTo(16f), assetPath);
                Assert.That(importer.filterMode, Is.EqualTo(FilterMode.Point), assetPath);
                Assert.That(importer.textureCompression,
                    Is.EqualTo(TextureImporterCompression.Uncompressed), assetPath);
                Assert.That(
                    AssetDatabase.LoadAllAssetRepresentationsAtPath(assetPath)
                        .OfType<Sprite>().Count(),
                    Is.EqualTo(16),
                    assetPath);
            }
        }

        private static void Configure(
            PresentationCatalog catalog,
            params (string Key, Object Asset)[] entries)
        {
            var serialized = new SerializedObject(catalog);
            SerializedProperty items = serialized.FindProperty("entries");
            items.arraySize = entries.Length;
            for (int index = 0; index < entries.Length; index++)
            {
                SerializedProperty item = items.GetArrayElementAtIndex(index);
                item.FindPropertyRelative("key").stringValue = entries[index].Key;
                item.FindPropertyRelative("asset").objectReferenceValue =
                    entries[index].Asset;
            }
            serialized.ApplyModifiedPropertiesWithoutUndo();
        }

        private static string Sha256(string path)
        {
            using (SHA256 sha = SHA256.Create())
            using (FileStream stream = File.OpenRead(path))
            {
                return BitConverter.ToString(sha.ComputeHash(stream))
                    .Replace("-", string.Empty)
                    .ToLowerInvariant();
            }
        }
    }
}
