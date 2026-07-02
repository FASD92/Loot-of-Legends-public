using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerBattleParticipantRendererTests
    {
        [Test]
        public void RenderParticipantsCreatesRemoteMarkerForOtherBattleMember()
        {
            GameObject root = new GameObject("BattleParticipants");
            try
            {
                PlayerBattleParticipantRenderer renderer =
                    root.AddComponent<PlayerBattleParticipantRenderer>();

                renderer.RenderParticipants(11UL, 11UL, 22UL);

                Assert.AreEqual(1, renderer.RemoteMarkerCount);
                GameObject marker = renderer.GetRemoteMarker(22UL);
                Assert.NotNull(marker);
                Assert.AreEqual("RemoteParticipant_22", marker.name);
                Assert.AreEqual(
                    PlayerBattleParticipantRenderer.DefaultRemotePosition,
                    marker.transform.localPosition);
                Assert.AreEqual(
                    Vector3.one * PlayerBattleParticipantRenderer.DefaultMarkerScale,
                    marker.transform.localScale);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderParticipantsClearsMarkersWhenSelfIsNotInBattle()
        {
            GameObject root = new GameObject("BattleParticipants");
            try
            {
                PlayerBattleParticipantRenderer renderer =
                    root.AddComponent<PlayerBattleParticipantRenderer>();
                renderer.RenderParticipants(11UL, 11UL, 22UL);

                renderer.RenderParticipants(33UL, 11UL, 22UL);

                Assert.AreEqual(0, renderer.RemoteMarkerCount);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderParticipantsUsesRelease0VisualProviderForRemoteMarker()
        {
            FakeVisualProvider fake = new FakeVisualProvider();
            Release0VisualProviders.Use(fake);
            GameObject root = new GameObject("BattleParticipants");
            try
            {
                PlayerBattleParticipantRenderer renderer =
                    root.AddComponent<PlayerBattleParticipantRenderer>();

                renderer.RenderParticipants(11UL, 11UL, 22UL);

                GameObject marker = renderer.GetRemoteMarker(22UL);
                Assert.AreEqual(1, fake.RemotePlayerCallCount);
                Assert.NotNull(marker);
                Assert.AreEqual("RemoteParticipant_22", marker.name);
                Assert.IsNull(marker.GetComponent<Collider>());
                Assert.IsNull(marker.GetComponentInChildren<Collider>());
                AssertChildRendererUsesMaterial(
                    marker,
                    "FakeRemotePlayerModel",
                    fake.RemotePlayerMaterial);
                AssertIdentityMarkerColor(
                    marker,
                    "RemoteParticipantIdentityMarker",
                    new Color(0.24f, 0.56f, 0.95f, 1.0f));
            }
            finally
            {
                Object.DestroyImmediate(root);
                Release0VisualProviders.Reset();
            }
        }

        [Test]
        public void RenderStateSnapshotParticipantsMovesRemoteMarkerToServerSnapshotPosition()
        {
            GameObject root = new GameObject("BattleParticipants");
            try
            {
                PlayerBattleParticipantRenderer renderer =
                    root.AddComponent<PlayerBattleParticipantRenderer>();
                PlayerRudpStateSnapshot snapshot = new PlayerRudpStateSnapshot(
                    17U,
                    8U,
                    new[]
                    {
                        new PlayerRudpStateSnapshotPlayer(11UL, 0, 0),
                        new PlayerRudpStateSnapshotPlayer(22UL, 1250, -500)
                    });

                renderer.RenderStateSnapshotParticipants(11UL, snapshot);

                GameObject marker = renderer.GetRemoteMarker(22UL);
                Assert.NotNull(marker);
                Assert.AreEqual(1.25f, marker.transform.localPosition.x, 0.0001f);
                Assert.AreEqual(1.1f, marker.transform.localPosition.y, 0.0001f);
                Assert.AreEqual(-0.5f, marker.transform.localPosition.z, 0.0001f);
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
            public readonly Material RemotePlayerMaterial =
                CreateTestMaterial("FakeRemotePlayerMaterial", Color.white);

            public int RemotePlayerCallCount;

            public GameObject CreateArenaFloor() => new GameObject("FakeArenaFloor");
            public GameObject CreateLocalPlayer() => new GameObject("FakeLocalPlayer");

            public GameObject CreateRemotePlayer()
            {
                ++RemotePlayerCallCount;
                GameObject root = new GameObject("FakeRemotePlayer");
                GameObject child = GameObject.CreatePrimitive(PrimitiveType.Capsule);
                child.name = "FakeRemotePlayerModel";
                child.transform.SetParent(root.transform, false);
                child.GetComponent<Renderer>().sharedMaterial = RemotePlayerMaterial;
                return root;
            }

            public GameObject CreateMonster() => new GameObject("FakeMonster");
            public GameObject CreateDrop() => new GameObject("FakeDrop");
        }
    }
}
