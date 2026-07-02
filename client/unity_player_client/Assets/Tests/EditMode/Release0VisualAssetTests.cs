using System.Collections.Generic;
using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using Object = UnityEngine.Object;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class Release0VisualAssetTests
    {
        [Test]
        public void RequiredRelease0VisualPrefabsAreBuildIncludedResources()
        {
            Assert.NotNull(Resources.Load<GameObject>(
                Release0ResourcesVisualProvider.DefaultArenaFloorPath));
            Assert.NotNull(Resources.Load<GameObject>(
                Release0ResourcesVisualProvider.DefaultLocalPlayerPath));
            Assert.NotNull(Resources.Load<GameObject>(
                Release0ResourcesVisualProvider.DefaultRemotePlayerPath));
            Assert.NotNull(Resources.Load<GameObject>(
                Release0ResourcesVisualProvider.DefaultMonsterPath));
            Assert.NotNull(Resources.Load<GameObject>(
                Release0ResourcesVisualProvider.DefaultDropPath));
        }

        [Test]
        public void RequiredKayKitTexturesAreImported()
        {
            Assert.NotNull(AssetDatabase.LoadAssetAtPath<Texture2D>(
                "Assets/ThirdParty/KayKit/DungeonRemastered/Assets/fbx-unity/dungeon_texture.png"));
            Assert.NotNull(AssetDatabase.LoadAssetAtPath<Texture2D>(
                "Assets/ThirdParty/KayKit/Adventurers/Characters/fbx/knight_texture.png"));
            Assert.NotNull(AssetDatabase.LoadAssetAtPath<Texture2D>(
                "Assets/ThirdParty/KayKit/Skeletons/characters/fbx/skeleton_texture.png"));
            Assert.NotNull(AssetDatabase.LoadAssetAtPath<Texture2D>(
                "Assets/ThirdParty/KayKit/ResourceBits/Assets/fbx/resource_bits_texture.png"));
        }

        [Test]
        public void RequiredQuaterniusMonsterAssetsAreImported()
        {
            Assert.NotNull(AssetDatabase.LoadAssetAtPath<GameObject>(
                "Assets/ThirdParty/Quaternius/UltimateMonsters/Big/FBX/BlueDemon.fbx"));
            Assert.NotNull(AssetDatabase.LoadAssetAtPath<Texture2D>(
                "Assets/ThirdParty/Quaternius/UltimateMonsters/Atlas_Monsters.png"));
        }

        [Test]
        public void RequiredKayKitMediumIdleAnimationIsImported()
        {
            Object[] assets = AssetDatabase.LoadAllAssetsAtPath(
                "Assets/ThirdParty/KayKit/Adventurers/Animations/fbx/Rig_Medium/Rig_Medium_General.fbx");
            AssertClipExists(assets, "Idle_A");
        }

        [Test]
        public void RequiredKayKitMediumMovementAnimationIsImported()
        {
            Object[] assets = AssetDatabase.LoadAllAssetsAtPath(
                "Assets/ThirdParty/KayKit/Adventurers/Animations/fbx/Rig_Medium/Rig_Medium_MovementBasic.fbx");
            AssertClipExists(assets, "Walking_A");
        }

        [Test]
        public void PlayerMovementClipsLoopForSustainedInput()
        {
            AssertClipLoops(
                "Assets/ThirdParty/KayKit/Adventurers/Animations/fbx/Rig_Medium/Rig_Medium_General.fbx",
                "Idle_A");
            AssertClipLoops(
                "Assets/ThirdParty/KayKit/Adventurers/Animations/fbx/Rig_Medium/Rig_Medium_MovementBasic.fbx",
                "Walking_A");
        }

        [Test]
        public void ResourceVisualsKeepKayKitTextureMaterials()
        {
            Release0ResourcesVisualProvider provider = new Release0ResourcesVisualProvider();
            GameObject arenaFloor = provider.CreateArenaFloor();
            GameObject localPlayer = provider.CreateLocalPlayer();
            GameObject remotePlayer = provider.CreateRemotePlayer();
            GameObject monster = provider.CreateMonster();
            GameObject drop = provider.CreateDrop();
            try
            {
                AssertRendererUsesTexture(arenaFloor, "dungeon_texture");
                AssertRendererUsesTexture(localPlayer, "knight_texture");
                AssertRendererUsesTexture(remotePlayer, "knight_texture");
                AssertRendererUsesTexture(monster, "Atlas_Monsters");
                AssertRendererUsesTexture(drop, "resource_bits_texture");
            }
            finally
            {
                Object.DestroyImmediate(arenaFloor);
                Object.DestroyImmediate(localPlayer);
                Object.DestroyImmediate(remotePlayer);
                Object.DestroyImmediate(monster);
                Object.DestroyImmediate(drop);
            }
        }

        [Test]
        public void ArenaFloorVisualUsesRepeatedKayKitTiles()
        {
            Release0ResourcesVisualProvider provider = new Release0ResourcesVisualProvider();
            GameObject arenaFloor = provider.CreateArenaFloor();
            try
            {
                Assert.NotNull(arenaFloor.GetComponent<Release0ArenaFloorVisual>());
                Renderer[] renderers = arenaFloor.GetComponentsInChildren<Renderer>(true);
                Assert.GreaterOrEqual(renderers.Length, 25);
                AssertHasRepeatedTilePositions(arenaFloor);
                AssertRendererUsesTexture(arenaFloor, "dungeon_texture");
            }
            finally
            {
                Object.DestroyImmediate(arenaFloor);
            }
        }

        [Test]
        public void DropVisualGoldNuggetModelHasVisibleBounds()
        {
            Release0ResourcesVisualProvider provider = new Release0ResourcesVisualProvider();
            GameObject drop = provider.CreateDrop();
            try
            {
                Transform model = drop.transform.Find("DropModel");
                Assert.NotNull(model);

                Bounds bounds = CalculateRendererBounds(model.gameObject);
                Assert.GreaterOrEqual(bounds.size.x, 0.45f);
                Assert.GreaterOrEqual(bounds.size.y, 0.20f);
                Assert.GreaterOrEqual(bounds.size.z, 0.40f);
                AssertRendererUsesTexture(drop, "resource_bits_texture");
            }
            finally
            {
                Object.DestroyImmediate(drop);
            }
        }

        [Test]
        public void MonsterVisualDefaultsToSouthFacingModelRotation()
        {
            Release0ResourcesVisualProvider provider = new Release0ResourcesVisualProvider();
            GameObject monster = provider.CreateMonster();
            try
            {
                Transform model = monster.transform.Find("MonsterModel");
                Assert.NotNull(model);

                Quaternion expected = Quaternion.Euler(0.0f, 180.0f, 0.0f);
                Assert.LessOrEqual(
                    Quaternion.Angle(expected, model.localRotation),
                    0.1f);
            }
            finally
            {
                Object.DestroyImmediate(monster);
            }
        }

        private static void AssertRendererUsesTexture(
            GameObject target,
            string expectedTextureName)
        {
            Renderer[] renderers = target.GetComponentsInChildren<Renderer>(true);
            foreach (Renderer renderer in renderers)
            {
                foreach (Material material in renderer.sharedMaterials)
                {
                    Texture texture = ReadMainTexture(material);
                    if (texture != null && texture.name == expectedTextureName)
                    {
                        return;
                    }
                }
            }

            Assert.Fail($"{target.name} does not use texture {expectedTextureName}");
        }

        private static void AssertHasRepeatedTilePositions(GameObject target)
        {
            HashSet<string> positions = new HashSet<string>();
            Renderer[] renderers = target.GetComponentsInChildren<Renderer>(true);
            foreach (Renderer renderer in renderers)
            {
                Vector3 position = renderer.transform.localPosition;
                positions.Add(
                    $"{Mathf.RoundToInt(position.x * 10.0f)}:" +
                    $"{Mathf.RoundToInt(position.z * 10.0f)}");
            }

            Assert.GreaterOrEqual(
                positions.Count,
                25,
                $"{target.name} should expose repeated tile positions");
        }

        [Test]
        public void LocalAndRemotePlayerVisualsUseMovementAnimatorController()
        {
            Release0ResourcesVisualProvider provider = new Release0ResourcesVisualProvider();
            GameObject localPlayer = provider.CreateLocalPlayer();
            GameObject remotePlayer = provider.CreateRemotePlayer();
            try
            {
                Assert.NotNull(localPlayer.GetComponent<Release0PlayerVisualAnimator>());
                Assert.NotNull(remotePlayer.GetComponent<Release0PlayerVisualAnimator>());
                AssertAnimatorUsesClip(localPlayer, "Idle_A", false);
                AssertAnimatorUsesClip(localPlayer, "Walking_A", true);
                AssertAnimatorUsesClip(remotePlayer, "Idle_A", false);
                AssertAnimatorUsesClip(remotePlayer, "Walking_A", true);
            }
            finally
            {
                Object.DestroyImmediate(localPlayer);
                Object.DestroyImmediate(remotePlayer);
            }
        }

        [Test]
        public void PlayerVisualAnimatorSetsMovementParameter()
        {
            Release0ResourcesVisualProvider provider = new Release0ResourcesVisualProvider();
            GameObject localPlayer = provider.CreateLocalPlayer();
            try
            {
                Release0PlayerVisualAnimator visualAnimator =
                    localPlayer.GetComponent<Release0PlayerVisualAnimator>();
                Assert.NotNull(visualAnimator);
                Animator animator = localPlayer.GetComponentInChildren<Animator>(true);
                Assert.NotNull(animator);

                visualAnimator.SetMoving(true);
                Assert.IsTrue(animator.GetBool(Release0PlayerVisualAnimator.IsMovingParameter));

                visualAnimator.SetMoving(false);
                Assert.IsFalse(animator.GetBool(Release0PlayerVisualAnimator.IsMovingParameter));
            }
            finally
            {
                Object.DestroyImmediate(localPlayer);
            }
        }

        private static void AssertClipExists(Object[] assets, string expectedClipName)
        {
            foreach (Object asset in assets)
            {
                AnimationClip clip = asset as AnimationClip;
                if (clip != null && clip.name == expectedClipName)
                {
                    return;
                }
            }

            Assert.Fail($"Animation clip {expectedClipName} was not imported");
        }

        private static void AssertClipLoops(string assetPath, string expectedClipName)
        {
            Object[] assets = AssetDatabase.LoadAllAssetsAtPath(assetPath);
            foreach (Object asset in assets)
            {
                AnimationClip clip = asset as AnimationClip;
                if (clip != null && clip.name == expectedClipName)
                {
                    AnimationClipSettings settings =
                        AnimationUtility.GetAnimationClipSettings(clip);
                    Assert.IsTrue(
                        settings.loopTime,
                        $"{expectedClipName} must loop for sustained movement input");
                    return;
                }
            }

            Assert.Fail($"Animation clip {expectedClipName} was not imported");
        }

        private static void AssertAnimatorUsesClip(
            GameObject target,
            string expectedClipName,
            bool requireHierarchySampling)
        {
            Animator animator = target.GetComponentInChildren<Animator>(true);
            Assert.NotNull(animator);
            Assert.NotNull(animator.runtimeAnimatorController);
            foreach (AnimationClip clip in animator.runtimeAnimatorController.animationClips)
            {
                if (clip != null && clip.name == expectedClipName)
                {
                    if (requireHierarchySampling)
                    {
                        AssertClipSamplesHierarchy(animator.gameObject, clip);
                    }
                    return;
                }
            }

            Assert.Fail($"{target.name} Animator does not use {expectedClipName}");
        }

        private static void AssertClipSamplesHierarchy(
            GameObject target,
            AnimationClip clip)
        {
            Transform[] transforms = target.GetComponentsInChildren<Transform>(true);
            Vector3[] positions = new Vector3[transforms.Length];
            Quaternion[] rotations = new Quaternion[transforms.Length];
            for (int index = 0; index < transforms.Length; ++index)
            {
                positions[index] = transforms[index].localPosition;
                rotations[index] = transforms[index].localRotation;
            }

            clip.SampleAnimation(target, 0.5f);

            for (int index = 0; index < transforms.Length; ++index)
            {
                if (Vector3.Distance(positions[index], transforms[index].localPosition) >
                    0.0001f ||
                    Quaternion.Angle(rotations[index], transforms[index].localRotation) >
                    0.01f)
                {
                    return;
                }
            }

            Assert.Fail($"{clip.name} did not affect {target.name} hierarchy");
        }

        private static Bounds CalculateRendererBounds(GameObject target)
        {
            Renderer[] renderers = target.GetComponentsInChildren<Renderer>(true);
            Assert.Greater(renderers.Length, 0, $"{target.name} should have renderers");

            Bounds bounds = renderers[0].bounds;
            for (int index = 1; index < renderers.Length; ++index)
            {
                bounds.Encapsulate(renderers[index].bounds);
            }

            return bounds;
        }

        private static Texture ReadMainTexture(Material material)
        {
            if (material == null)
            {
                return null;
            }

            if (material.HasProperty("_BaseMap"))
            {
                return material.GetTexture("_BaseMap");
            }

            return material.HasProperty("_MainTex") ?
                material.GetTexture("_MainTex") :
                material.mainTexture;
        }
    }
}
