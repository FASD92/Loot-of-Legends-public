using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class Release0VisualProviderTests
    {
        [TearDown]
        public void TearDown()
        {
            Release0VisualProviders.Reset();
        }

        [Test]
        public void DefaultProviderCreatesPrimitiveFallbacksWhenResourcesAreMissing()
        {
            IRelease0VisualProvider provider = new Release0ResourcesVisualProvider(
                "Release0Visuals/MissingArena",
                "Release0Visuals/MissingLocalPlayer",
                "Release0Visuals/MissingRemotePlayer",
                "Release0Visuals/MissingMonster",
                "Release0Visuals/MissingDrop");

            GameObject arena = provider.CreateArenaFloor();
            GameObject local = provider.CreateLocalPlayer();
            GameObject remote = provider.CreateRemotePlayer();
            GameObject monster = provider.CreateMonster();
            GameObject drop = provider.CreateDrop();

            try
            {
                Assert.AreEqual("ArenaFloor", arena.name);
                Assert.AreEqual("PlayerAvatar", local.name);
                Assert.AreEqual("RemoteParticipant", remote.name);
                Assert.AreEqual("Monster", monster.name);
                Assert.AreEqual("DropMarker", drop.name);
                Assert.NotNull(arena.GetComponent<Renderer>());
                Assert.NotNull(local.GetComponent<Renderer>());
                Assert.NotNull(remote.GetComponent<Renderer>());
                Assert.NotNull(monster.GetComponent<Renderer>());
                Assert.NotNull(drop.GetComponent<Renderer>());
            }
            finally
            {
                Object.DestroyImmediate(arena);
                Object.DestroyImmediate(local);
                Object.DestroyImmediate(remote);
                Object.DestroyImmediate(monster);
                Object.DestroyImmediate(drop);
            }
        }

        [Test]
        public void CurrentProviderCanBeReplacedAndReset()
        {
            FakeVisualProvider fake = new FakeVisualProvider();

            Release0VisualProviders.Use(fake);
            Assert.AreSame(fake, Release0VisualProviders.Current);

            Release0VisualProviders.Reset();
            Assert.IsInstanceOf<Release0ResourcesVisualProvider>(
                Release0VisualProviders.Current);
        }

        [Test]
        public void DefaultResourcePathsPointToRelease0VisualEntrypoints()
        {
            Assert.AreEqual(
                "Release0Visuals/ArenaFloor",
                Release0ResourcesVisualProvider.DefaultArenaFloorPath);
            Assert.AreEqual(
                "Release0Visuals/LocalPlayer",
                Release0ResourcesVisualProvider.DefaultLocalPlayerPath);
            Assert.AreEqual(
                "Release0Visuals/RemotePlayer",
                Release0ResourcesVisualProvider.DefaultRemotePlayerPath);
            Assert.AreEqual(
                "Release0Visuals/Monster",
                Release0ResourcesVisualProvider.DefaultMonsterPath);
            Assert.AreEqual(
                "Release0Visuals/Drop",
                Release0ResourcesVisualProvider.DefaultDropPath);
        }

        private sealed class FakeVisualProvider : IRelease0VisualProvider
        {
            public GameObject CreateArenaFloor() => new GameObject("FakeArena");
            public GameObject CreateLocalPlayer() => new GameObject("FakeLocal");
            public GameObject CreateRemotePlayer() => new GameObject("FakeRemote");
            public GameObject CreateMonster() => new GameObject("FakeMonster");
            public GameObject CreateDrop() => new GameObject("FakeDrop");
        }
    }
}
