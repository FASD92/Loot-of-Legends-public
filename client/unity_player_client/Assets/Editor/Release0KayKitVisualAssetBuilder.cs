#if UNITY_EDITOR
using System.IO;
using UnityEditor;
using UnityEditor.Animations;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Editor
{
    public static class Release0KayKitVisualAssetBuilder
    {
        private const string ResourceRoot = "Assets/Resources/Release0Visuals";
        private const string MediumGeneralAnimationPath =
            "Assets/ThirdParty/KayKit/Adventurers/Animations/fbx/Rig_Medium/Rig_Medium_General.fbx";
        private const string MediumMovementAnimationPath =
            "Assets/ThirdParty/KayKit/Adventurers/Animations/fbx/Rig_Medium/Rig_Medium_MovementBasic.fbx";
        private const string KnightPlayerModelPath =
            "Assets/ThirdParty/KayKit/Adventurers/Characters/fbx/Knight.fbx";
        private const string ArenaFloorModelPath =
            "Assets/ThirdParty/KayKit/DungeonRemastered/Assets/fbx-unity/floor_tile_large.fbx";
        private const string BlueDemonMonsterModelPath =
            "Assets/ThirdParty/Quaternius/UltimateMonsters/Big/FBX/BlueDemon.fbx";
        private const string MediumMovementControllerPath =
            ResourceRoot + "/Release0MediumMovement.controller";
        private static readonly Vector3 BlueDemonMonsterModelScale =
            Vector3.one * 0.85f;
        private static readonly Quaternion BlueDemonMonsterModelRotation =
            Quaternion.Euler(0.0f, 180.0f, 0.0f);
        private static readonly Vector3 DropModelScale =
            Vector3.one * 100.0f;
        private const int ArenaFloorTileRadius = 5;
        private const float ArenaFloorTileSpacing = 10.0f;
        private const float ArenaFloorTileScale = 5.0f;

        public static void Build()
        {
            Directory.CreateDirectory(ResourceRoot);
            SaveIdleController();
            SaveArenaFloor();
            SaveWrapper(
                "LocalPlayer",
                KnightPlayerModelPath,
                Vector3.one,
                MediumMovementControllerPath);
            SaveWrapper(
                "RemotePlayer",
                KnightPlayerModelPath,
                Vector3.one,
                MediumMovementControllerPath);
            SaveWrapper(
                "Monster",
                BlueDemonMonsterModelPath,
                BlueDemonMonsterModelScale,
                BlueDemonMonsterModelRotation);
            SaveWrapper(
                "Drop",
                "Assets/ThirdParty/KayKit/ResourceBits/Assets/fbx/Gold_Nuggets.fbx",
                DropModelScale);
            AssetDatabase.SaveAssets();
            AssetDatabase.Refresh();
        }

        private static void SaveWrapper(
            string name,
            string modelPath,
            Vector3 scale,
            string animatorControllerPath = null)
        {
            SaveWrapper(
                name,
                modelPath,
                scale,
                Quaternion.identity,
                animatorControllerPath);
        }

        private static void SaveWrapper(
            string name,
            string modelPath,
            Vector3 scale,
            Quaternion localRotation,
            string animatorControllerPath = null)
        {
            GameObject model = AssetDatabase.LoadAssetAtPath<GameObject>(modelPath);
            if (model == null)
            {
                throw new FileNotFoundException(modelPath);
            }

            GameObject wrapper = new GameObject(name);
            GameObject child = (GameObject)PrefabUtility.InstantiatePrefab(model);
            child.name = $"{name}Model";
            child.transform.SetParent(wrapper.transform, false);
            child.transform.localScale = scale;
            child.transform.localRotation = localRotation;
            ConfigureAnimator(child, modelPath, animatorControllerPath);
            PrefabUtility.SaveAsPrefabAsset(wrapper, $"{ResourceRoot}/{name}.prefab");
            Object.DestroyImmediate(wrapper);
        }

        private static void SaveArenaFloor()
        {
            GameObject model = AssetDatabase.LoadAssetAtPath<GameObject>(ArenaFloorModelPath);
            if (model == null)
            {
                throw new FileNotFoundException(ArenaFloorModelPath);
            }

            GameObject wrapper = new GameObject("ArenaFloor");
            wrapper.AddComponent<Release0ArenaFloorVisual>();
            for (int z = -ArenaFloorTileRadius; z <= ArenaFloorTileRadius; ++z)
            {
                for (int x = -ArenaFloorTileRadius; x <= ArenaFloorTileRadius; ++x)
                {
                    GameObject tile = (GameObject)PrefabUtility.InstantiatePrefab(model);
                    tile.name = $"ArenaFloorTile_{x}_{z}";
                    tile.transform.SetParent(wrapper.transform, false);
                    tile.transform.localPosition =
                        new Vector3(
                            x * ArenaFloorTileSpacing,
                            0.0f,
                            z * ArenaFloorTileSpacing);
                    tile.transform.localRotation =
                        Quaternion.Euler(0.0f, ((x + z) & 3) * 90.0f, 0.0f);
                    tile.transform.localScale =
                        new Vector3(
                            ArenaFloorTileScale,
                            1.0f,
                            ArenaFloorTileScale);
                }
            }

            PrefabUtility.SaveAsPrefabAsset(wrapper, $"{ResourceRoot}/ArenaFloor.prefab");
            Object.DestroyImmediate(wrapper);
        }

        private static void SaveIdleController()
        {
            EnsureAnimationClipLoops(MediumGeneralAnimationPath, "Idle_A");
            EnsureAnimationClipLoops(MediumMovementAnimationPath, "Walking_A");

            AnimationClip idleClip = LoadAnimationClip(
                MediumGeneralAnimationPath,
                "Idle_A");
            if (idleClip == null)
            {
                throw new FileNotFoundException(
                    $"Idle_A clip not found in {MediumGeneralAnimationPath}");
            }
            AnimationClip walkingClip = LoadAnimationClip(
                MediumMovementAnimationPath,
                "Walking_A");
            if (walkingClip == null)
            {
                throw new FileNotFoundException(
                    $"Walking_A clip not found in {MediumMovementAnimationPath}");
            }

            AssetDatabase.DeleteAsset(MediumMovementControllerPath);
            AnimatorController controller =
                AnimatorController.CreateAnimatorControllerAtPath(
                    MediumMovementControllerPath);
            controller.AddParameter(
                Release0PlayerVisualAnimator.IsMovingParameter,
                AnimatorControllerParameterType.Bool);
            AnimatorStateMachine stateMachine = controller.layers[0].stateMachine;
            AnimatorState idleState = stateMachine.AddState("Idle_A");
            idleState.motion = idleClip;
            AnimatorState walkingState = stateMachine.AddState("Walking_A");
            walkingState.motion = walkingClip;
            stateMachine.defaultState = idleState;

            AnimatorStateTransition idleToWalking = idleState.AddTransition(walkingState);
            idleToWalking.hasExitTime = false;
            idleToWalking.duration = 0.1f;
            idleToWalking.AddCondition(
                AnimatorConditionMode.If,
                0.0f,
                Release0PlayerVisualAnimator.IsMovingParameter);

            AnimatorStateTransition walkingToIdle = walkingState.AddTransition(idleState);
            walkingToIdle.hasExitTime = false;
            walkingToIdle.duration = 0.1f;
            walkingToIdle.AddCondition(
                AnimatorConditionMode.IfNot,
                0.0f,
                Release0PlayerVisualAnimator.IsMovingParameter);
        }

        private static void EnsureAnimationClipLoops(
            string assetPath,
            string clipName)
        {
            ModelImporter importer = AssetImporter.GetAtPath(assetPath) as ModelImporter;
            if (importer == null)
            {
                throw new FileNotFoundException(assetPath);
            }

            ModelImporterClipAnimation[] clips = importer.clipAnimations;
            if (clips == null || clips.Length == 0)
            {
                clips = importer.defaultClipAnimations;
            }

            bool found = false;
            bool changed = false;
            for (int index = 0; index < clips.Length; ++index)
            {
                ModelImporterClipAnimation clip = clips[index];
                if (clip.name != clipName)
                {
                    continue;
                }

                found = true;
                if (!clip.loopTime || clip.wrapMode != WrapMode.Loop)
                {
                    clip.loopTime = true;
                    clip.wrapMode = WrapMode.Loop;
                    clips[index] = clip;
                    changed = true;
                }
            }

            if (!found)
            {
                throw new FileNotFoundException(
                    $"{clipName} clip not found in {assetPath}");
            }

            if (changed)
            {
                importer.clipAnimations = clips;
                importer.SaveAndReimport();
            }
        }

        private static void ConfigureAnimator(
            GameObject child,
            string modelPath,
            string animatorControllerPath)
        {
            if (string.IsNullOrEmpty(animatorControllerPath))
            {
                return;
            }

            RuntimeAnimatorController controller =
                AssetDatabase.LoadAssetAtPath<RuntimeAnimatorController>(
                    animatorControllerPath);
            if (controller == null)
            {
                throw new FileNotFoundException(animatorControllerPath);
            }

            Animator animator = child.GetComponent<Animator>();
            if (animator == null)
            {
                animator = child.GetComponentInChildren<Animator>(true);
            }
            if (animator == null)
            {
                animator = child.AddComponent<Animator>();
            }
            if (animator.avatar == null)
            {
                animator.avatar = LoadAvatar(modelPath);
            }

            animator.runtimeAnimatorController = controller;
            animator.applyRootMotion = false;

            Release0PlayerVisualAnimator visualAnimator =
                child.transform.parent.GetComponent<Release0PlayerVisualAnimator>();
            if (visualAnimator == null)
            {
                visualAnimator =
                    child.transform.parent.gameObject.AddComponent<Release0PlayerVisualAnimator>();
            }

            visualAnimator.Animator = animator;
        }

        private static AnimationClip LoadAnimationClip(
            string assetPath,
            string clipName)
        {
            Object[] assets = AssetDatabase.LoadAllAssetsAtPath(assetPath);
            for (int index = 0; index < assets.Length; ++index)
            {
                AnimationClip clip = assets[index] as AnimationClip;
                if (clip != null && clip.name == clipName)
                {
                    return clip;
                }
            }

            return null;
        }

        private static Avatar LoadAvatar(string modelPath)
        {
            Object[] assets = AssetDatabase.LoadAllAssetsAtPath(modelPath);
            for (int index = 0; index < assets.Length; ++index)
            {
                Avatar avatar = assets[index] as Avatar;
                if (avatar != null)
                {
                    return avatar;
                }
            }

            return null;
        }
    }
}
#endif
