using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerMonsterMarkerRendererTests
    {
        [Test]
        public void RenderMonsterCreatesVisibleMarkerAtArenaCenter()
        {
            FakeVisualProvider fake = new FakeVisualProvider();
            Release0VisualProviders.Use(fake);
            GameObject root = new GameObject("MonsterMarker");
            try
            {
                PlayerMonsterMarkerRenderer renderer =
                    root.AddComponent<PlayerMonsterMarkerRenderer>();

                renderer.RenderMonster(17U, 1001U, 2001U, 30);

                Assert.AreEqual(1, fake.MonsterCallCount);
                Assert.IsTrue(renderer.HasMarker);
                Assert.NotNull(renderer.Marker);
                Assert.AreEqual("Monster_1001", renderer.Marker.name);
                Assert.IsNull(renderer.Marker.GetComponent<Collider>());
                Assert.IsNull(renderer.Marker.GetComponentInChildren<Collider>());
                AssertChildRendererUsesMaterial(
                    renderer.Marker,
                    "FakeMonsterModel",
                    fake.MonsterMaterial);
                AssertIdentityMarkerColor(
                    renderer.Marker,
                    "MonsterIdentityMarker",
                    new Color(0.88f, 0.24f, 0.20f, 1.0f));
                Assert.AreEqual(
                    PlayerMonsterMarkerRenderer.DefaultMonsterPosition,
                    renderer.Marker.transform.localPosition);
                Assert.AreEqual(
                    Vector3.one * PlayerMonsterMarkerRenderer.DefaultMonsterScale,
                    renderer.Marker.transform.localScale);
            }
            finally
            {
                Object.DestroyImmediate(root);
                Release0VisualProviders.Reset();
            }
        }

        [Test]
        public void RenderMonsterShowsWorldSpaceHealthBarAboveMarker()
        {
            GameObject root = new GameObject("MonsterMarker");
            try
            {
                PlayerMonsterMarkerRenderer renderer =
                    root.AddComponent<PlayerMonsterMarkerRenderer>();

                renderer.RenderMonster(17U, 1001U, 2001U, 75, 100);

                Assert.IsTrue(renderer.HasMarker);
                Assert.NotNull(renderer.HealthBar);
                Assert.AreEqual(0.75f, renderer.HealthBar.FillRatio, 0.0001f);
                Assert.Greater(
                    renderer.HealthBar.transform.localPosition.y,
                    renderer.Marker.transform.localPosition.y);
                Assert.AreEqual(
                    PlayerMonsterMarkerRenderer.HealthBarLocalPosition,
                    renderer.HealthBar.transform.localPosition);
                Assert.That(
                    Quaternion.Angle(
                        PlayerMonsterMarkerRenderer.HealthBarLocalRotation,
                        renderer.HealthBar.transform.localRotation),
                    Is.LessThan(0.01f));
                Assert.AreEqual(
                    PlayerMonsterMarkerRenderer.HealthBarLocalScale,
                    renderer.HealthBar.transform.localScale);
                Assert.IsNull(renderer.Marker.GetComponentInChildren<TextMesh>());
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderMonsterClearsMarkerForInvalidMonsterState()
        {
            GameObject root = new GameObject("MonsterMarker");
            try
            {
                PlayerMonsterMarkerRenderer renderer =
                    root.AddComponent<PlayerMonsterMarkerRenderer>();
                renderer.RenderMonster(17U, 1001U, 2001U, 30);

                renderer.RenderMonster(17U, 0U, 2001U, 30);

                Assert.IsFalse(renderer.HasMarker);
                Assert.IsNull(renderer.Marker);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
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
            public readonly Material MonsterMaterial =
                CreateTestMaterial("FakeMonsterMaterial", Color.white);

            public int MonsterCallCount;

            public GameObject CreateArenaFloor() => new GameObject("FakeArenaFloor");
            public GameObject CreateLocalPlayer() => new GameObject("FakeLocalPlayer");
            public GameObject CreateRemotePlayer() => new GameObject("FakeRemotePlayer");

            public GameObject CreateMonster()
            {
                ++MonsterCallCount;
                GameObject root = new GameObject("FakeMonster");
                GameObject child = GameObject.CreatePrimitive(PrimitiveType.Sphere);
                child.name = "FakeMonsterModel";
                child.transform.SetParent(root.transform, false);
                child.GetComponent<Renderer>().sharedMaterial = MonsterMaterial;
                return root;
            }

            public GameObject CreateDrop() => new GameObject("FakeDrop");
        }
    }
}
