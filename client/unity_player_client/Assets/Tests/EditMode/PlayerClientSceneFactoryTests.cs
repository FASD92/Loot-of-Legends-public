using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerClientSceneFactoryTests
    {
        [Test]
        public void BuildResultDoesNotExposeRetiredDebugSurfaceFields()
        {
            Assert.IsNull(
                typeof(PlayerClientSceneObjects).GetField("InventoryActionPreviewRenderer"));
            Assert.IsNull(typeof(PlayerClientSceneObjects).GetField("ManualCommandPanel"));
            Assert.IsNull(typeof(PlayerClientSceneObjects).GetField("ManualBattleCommandPanel"));
            Assert.IsNull(typeof(PlayerClientSceneObjects).GetField("ManualCombatCommandPanel"));
            Assert.IsNull(typeof(PlayerClientSceneObjects).GetField("ManualLootCommandPanel"));
            Assert.IsNull(
                typeof(PlayerClientSceneObjects).GetField("ManualSmokeChecklistRenderer"));
        }

        [Test]
        public void BuildCreatesRelease0ArenaPlayerCameraAndLightWithoutLegacyRoomPanel()
        {
            GameObject root = new GameObject("TestRoot");
            try
            {
                PlayerClientSceneObjects objects = PlayerClientSceneFactory.Build(root.transform);

                Assert.NotNull(objects.ArenaFloor);
                Assert.NotNull(objects.PlayerAvatar);
                Assert.NotNull(objects.MainCamera);
                Assert.NotNull(objects.DirectionalLight);
                Assert.NotNull(objects.PlayerController);
                Assert.NotNull(objects.CameraFollow);
                Assert.NotNull(objects.NetworkSessionController);
                Assert.NotNull(objects.BattleParticipantRenderer);
                Assert.NotNull(objects.MonsterMarkerRenderer);
                Assert.NotNull(objects.DropMarkerRenderer);
                Assert.NotNull(objects.InventoryStatusRenderer);
                Assert.NotNull(objects.LootRaceEvidenceRenderer);
                Assert.AreEqual("ArenaFloor", objects.ArenaFloor.name);
                Assert.AreEqual("PlayerAvatar", objects.PlayerAvatar.name);
                Assert.AreEqual(PrimitiveType.Cube, objects.ArenaFloorPrimitiveType);
                Assert.AreEqual(PrimitiveType.Capsule, objects.PlayerAvatarPrimitiveType);
                AssertIdentityMarkerColor(
                    objects.PlayerAvatar,
                    "LocalPlayerIdentityMarker",
                    new Color(0.94f, 0.72f, 0.28f, 1.0f));
                Assert.AreSame(objects.PlayerAvatar, objects.PlayerController.gameObject);
                Assert.AreSame(objects.PlayerAvatar.transform, objects.CameraFollow.Target);
                Assert.AreSame(root, objects.NetworkSessionController.gameObject);
                Assert.AreSame(
                    objects.NetworkSessionController,
                    objects.PlayerController.NetworkSessionController);
                Assert.AreSame(root.transform, objects.BattleParticipantRenderer.transform.parent);
                Assert.AreSame(
                    objects.NetworkSessionController,
                    objects.BattleParticipantRenderer.NetworkSessionController);
                Assert.AreSame(root.transform, objects.MonsterMarkerRenderer.transform.parent);
                Assert.AreSame(
                    objects.NetworkSessionController,
                    objects.MonsterMarkerRenderer.NetworkSessionController);
                Assert.AreSame(root.transform, objects.DropMarkerRenderer.transform.parent);
                Assert.AreSame(
                    objects.NetworkSessionController,
                    objects.DropMarkerRenderer.NetworkSessionController);
                Assert.AreSame(root.transform, objects.InventoryStatusRenderer.transform.parent);
                Assert.AreSame(
                    objects.NetworkSessionController,
                    objects.InventoryStatusRenderer.NetworkSessionController);
                Assert.AreSame(
                    root.transform,
                    objects.LootRaceEvidenceRenderer.transform.parent);
                Assert.AreSame(
                    objects.NetworkSessionController,
                    objects.LootRaceEvidenceRenderer.NetworkSessionController);
                AssertRetiredDebugObjectsAbsent();
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, objects.NetworkSessionController.Status);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void BuildUsesRelease0VisualProviderForArenaAndLocalPlayer()
        {
            FakeVisualProvider fake = new FakeVisualProvider();
            Release0VisualProviders.Use(fake);
            GameObject root = new GameObject("TestRoot");
            try
            {
                PlayerClientSceneObjects objects = PlayerClientSceneFactory.Build(root.transform);

                Assert.AreEqual(1, fake.ArenaFloorCallCount);
                Assert.AreEqual(1, fake.LocalPlayerCallCount);
                Assert.AreEqual(0, fake.RemotePlayerCallCount);
                Assert.AreEqual(0, fake.MonsterCallCount);
                Assert.AreEqual(0, fake.DropCallCount);
                Assert.AreEqual("ArenaFloor", objects.ArenaFloor.name);
                Assert.AreEqual("PlayerAvatar", objects.PlayerAvatar.name);
                Assert.IsNull(objects.ArenaFloor.GetComponent<Collider>());
                Assert.IsNull(objects.ArenaFloor.GetComponentInChildren<Collider>());
                Assert.IsNull(objects.PlayerAvatar.GetComponent<Collider>());
                Assert.IsNull(objects.PlayerAvatar.GetComponentInChildren<Collider>());
                AssertChildRendererUsesMaterial(
                    objects.ArenaFloor,
                    "FakeArenaFloorModel",
                    fake.ArenaFloorMaterial);
                AssertChildRendererUsesMaterial(
                    objects.PlayerAvatar,
                    "FakeLocalPlayerModel",
                    fake.LocalPlayerMaterial);
                AssertIdentityMarkerColor(
                    objects.PlayerAvatar,
                    "LocalPlayerIdentityMarker",
                    new Color(0.94f, 0.72f, 0.28f, 1.0f));
                Assert.AreSame(objects.PlayerAvatar, objects.PlayerController.gameObject);
                Assert.AreSame(objects.PlayerAvatar.transform, objects.CameraFollow.Target);
            }
            finally
            {
                Object.DestroyImmediate(root);
                Release0VisualProviders.Reset();
            }
        }

        [Test]
        public void BuildKeepsWorldScaleArenaFloorPrefabUnstretched()
        {
            Release0VisualProviders.Reset();
            GameObject root = new GameObject("TestRoot");
            try
            {
                PlayerClientSceneObjects objects = PlayerClientSceneFactory.Build(root.transform);

                Assert.NotNull(objects.ArenaFloor.GetComponent<Release0ArenaFloorVisual>());
                Assert.AreEqual(Vector3.one, objects.ArenaFloor.transform.localScale);
                Assert.GreaterOrEqual(
                    objects.ArenaFloor.GetComponentsInChildren<Renderer>(true).Length,
                    25);
            }
            finally
            {
                Object.DestroyImmediate(root);
                Release0VisualProviders.Reset();
            }
        }

        [Test]
        public void BuildUsesBuildIncludedSceneMaterialAssetsForPrimitiveFallback()
        {
            Material arenaMaterialAsset =
                Resources.Load<Material>("Materials/ArenaFloorMaterial");
            Material playerMaterialAsset =
                Resources.Load<Material>("Materials/PlayerAvatarMaterial");
            Assert.NotNull(arenaMaterialAsset);
            Assert.NotNull(playerMaterialAsset);

            Release0VisualProviders.Use(new PrimitiveFallbackVisualProvider());
            GameObject root = new GameObject("TestRoot");
            try
            {
                PlayerClientSceneObjects objects = PlayerClientSceneFactory.Build(root.transform);

                AssertUsesMaterialAsset(objects.ArenaFloor, arenaMaterialAsset);
                AssertUsesMaterialAsset(objects.PlayerAvatar, playerMaterialAsset);
                Assert.AreEqual(
                    PlayerLocalMovement.DefaultArenaClampHalfExtent * 2.0f,
                    objects.ArenaFloor.transform.localScale.x,
                    0.0001f);
                Assert.AreEqual(
                    PlayerLocalMovement.DefaultArenaClampHalfExtent * 2.0f,
                    objects.ArenaFloor.transform.localScale.z,
                    0.0001f);
            }
            finally
            {
                Object.DestroyImmediate(root);
                Release0VisualProviders.Reset();
            }
        }

        private static void AssertRetiredDebugObjectsAbsent()
        {
            Assert.IsNull(GameObject.Find("InventoryActionPreview"));
            Assert.IsNull(GameObject.Find("ManualCommandPanel"));
            Assert.IsNull(GameObject.Find("ManualBattleCommandPanel"));
            Assert.IsNull(GameObject.Find("ManualCombatCommandPanel"));
            Assert.IsNull(GameObject.Find("ManualLootCommandPanel"));
            Assert.IsNull(GameObject.Find("ManualSmokeChecklist"));
        }

        private static void AssertChildRendererUsesMaterial(
            GameObject target,
            string childName,
            Material expectedMaterial)
        {
            Transform child = target.transform.Find(childName);
            Assert.NotNull(child);
            Renderer renderer = child.GetComponent<Renderer>();
            Assert.NotNull(renderer);
            Assert.AreSame(expectedMaterial, renderer.sharedMaterial);
        }

        private static void AssertIdentityMarkerColor(
            GameObject target,
            string markerName,
            Color expectedColor)
        {
            Transform marker = target.transform.Find(markerName);
            Assert.NotNull(marker);
            Renderer renderer = marker.GetComponent<Renderer>();
            Assert.NotNull(renderer);
            Assert.NotNull(renderer.sharedMaterial);
            Color actualColor = ReadMaterialColor(renderer.sharedMaterial);
            Assert.That(actualColor.r, Is.EqualTo(expectedColor.r).Within(0.001f));
            Assert.That(actualColor.g, Is.EqualTo(expectedColor.g).Within(0.001f));
            Assert.That(actualColor.b, Is.EqualTo(expectedColor.b).Within(0.001f));
            Assert.That(actualColor.a, Is.EqualTo(expectedColor.a).Within(0.001f));
        }

        private static void AssertUsesMaterialAsset(GameObject target, Material materialAsset)
        {
            Renderer renderer = target.GetComponentInChildren<Renderer>();
            Assert.NotNull(renderer);
            Assert.NotNull(renderer.sharedMaterial);
            Assert.AreSame(materialAsset.shader, renderer.sharedMaterial.shader);
            Assert.AreNotSame(materialAsset, renderer.sharedMaterial);
        }

        private static Color ReadMaterialColor(Material material)
        {
            if (material.HasProperty("_BaseColor"))
            {
                return material.GetColor("_BaseColor");
            }

            return material.HasProperty("_Color") ?
                material.GetColor("_Color") :
                material.color;
        }

        private static Material CreateTestMaterial(string materialName, Color color)
        {
            Shader shader =
                Shader.Find("Standard") ??
                Shader.Find("Universal Render Pipeline/Lit") ??
                Shader.Find("Universal Render Pipeline/Unlit");
            Material material = new Material(shader);
            material.name = materialName;
            if (material.HasProperty("_BaseColor"))
            {
                material.SetColor("_BaseColor", color);
            }
            if (material.HasProperty("_Color"))
            {
                material.SetColor("_Color", color);
            }

            return material;
        }

        private sealed class FakeVisualProvider : IRelease0VisualProvider
        {
            public readonly Material ArenaFloorMaterial =
                CreateTestMaterial("FakeArenaFloorMaterial", Color.gray);
            public readonly Material LocalPlayerMaterial =
                CreateTestMaterial("FakeLocalPlayerMaterial", Color.white);

            public int ArenaFloorCallCount;
            public int LocalPlayerCallCount;
            public int RemotePlayerCallCount;
            public int MonsterCallCount;
            public int DropCallCount;

            public GameObject CreateArenaFloor()
            {
                ++ArenaFloorCallCount;
                return CreateVisualWithChildPrimitive(
                    "FakeArenaFloor",
                    PrimitiveType.Cube,
                    ArenaFloorMaterial);
            }

            public GameObject CreateLocalPlayer()
            {
                ++LocalPlayerCallCount;
                return CreateVisualWithChildPrimitive(
                    "FakeLocalPlayer",
                    PrimitiveType.Capsule,
                    LocalPlayerMaterial);
            }

            public GameObject CreateRemotePlayer()
            {
                ++RemotePlayerCallCount;
                return GameObject.CreatePrimitive(PrimitiveType.Capsule);
            }

            public GameObject CreateMonster()
            {
                ++MonsterCallCount;
                return GameObject.CreatePrimitive(PrimitiveType.Sphere);
            }

            public GameObject CreateDrop()
            {
                ++DropCallCount;
                return GameObject.CreatePrimitive(PrimitiveType.Sphere);
            }

            private static GameObject CreateVisualWithChildPrimitive(
                string objectName,
                PrimitiveType primitiveType,
                Material material)
            {
                GameObject root = new GameObject(objectName);
                GameObject child = GameObject.CreatePrimitive(primitiveType);
                child.name = $"{objectName}Model";
                child.transform.SetParent(root.transform, false);
                child.GetComponent<Renderer>().sharedMaterial = material;
                return root;
            }
        }

        private sealed class PrimitiveFallbackVisualProvider : IRelease0VisualProvider
        {
            public GameObject CreateArenaFloor() => GameObject.CreatePrimitive(PrimitiveType.Cube);
            public GameObject CreateLocalPlayer() => GameObject.CreatePrimitive(PrimitiveType.Capsule);
            public GameObject CreateRemotePlayer() => new GameObject("FakeRemotePlayer");
            public GameObject CreateMonster() => new GameObject("FakeMonster");
            public GameObject CreateDrop() => new GameObject("FakeDrop");
        }
    }
}
