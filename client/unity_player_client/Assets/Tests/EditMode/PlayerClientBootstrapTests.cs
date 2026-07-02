using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerClientBootstrapTests
    {
        [SetUp]
        public void SetUp()
        {
            DestroyBootstrapRoots();
        }

        [TearDown]
        public void TearDown()
        {
            DestroyBootstrapRoots();
        }

        [Test]
        public void EnsureBootstrappedForRuntimeCreatesPlayerClientRootWhenSceneHasNoBootstrap()
        {
            try
            {
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();

                PlayerClientBootstrap bootstrap = Object.FindAnyObjectByType<PlayerClientBootstrap>();
                Assert.NotNull(bootstrap);
                Assert.AreEqual("PlayerClientRoot", bootstrap.gameObject.name);
                Assert.NotNull(Object.FindAnyObjectByType<PlayerNetworkSessionController>());
                AssertRetiredDebugObjectsAbsent();
                Assert.IsNull(GameObject.Find("ArenaFloor"));
                Assert.IsNull(GameObject.Find("PlayerAvatar"));
            }
            finally
            {
                DestroyBootstrapRoots();
            }
        }

        [TestCase("LobbyScene")]
        [TestCase("RoomScene")]
        public void LoginThenRelease0SceneLoadedBuildsRuntimeSessionWithoutLegacyRoomPanel(
            string release0SceneName)
        {
            Scene previousScene = SceneManager.GetActiveScene();
            Scene loginScene = EditorSceneManager.OpenScene(
                "Assets/Scenes/LoginScene.unity",
                OpenSceneMode.Additive);
            Scene release0Scene = EditorSceneManager.OpenScene(
                $"Assets/Scenes/{release0SceneName}.unity",
                OpenSceneMode.Additive);

            try
            {
                Assert.True(SceneManager.SetActiveScene(loginScene));
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();
                PlayerClientBootstrap bootstrap = Object.FindAnyObjectByType<PlayerClientBootstrap>();

                Assert.NotNull(bootstrap);
                Assert.Null(Object.FindAnyObjectByType<PlayerNetworkSessionController>());

                Assert.True(SceneManager.SetActiveScene(release0Scene));
                bootstrap.HandleSceneLoadedForTests(release0Scene, LoadSceneMode.Single);

                Assert.NotNull(Object.FindAnyObjectByType<PlayerNetworkSessionController>());
                AssertRetiredDebugObjectsAbsent();
                Assert.IsNull(GameObject.Find("ArenaFloor"));
                Assert.IsNull(GameObject.Find("PlayerAvatar"));
            }
            finally
            {
                DestroyBootstrapRoots();
                if (previousScene.IsValid())
                {
                    SceneManager.SetActiveScene(previousScene);
                }

                EditorSceneManager.CloseScene(loginScene, true);
                EditorSceneManager.CloseScene(release0Scene, true);
            }
        }

        [Test]
        public void LoginThenArenaSceneLoadedBuildsArenaRuntime()
        {
            Scene previousScene = SceneManager.GetActiveScene();
            Scene loginScene = EditorSceneManager.OpenScene(
                "Assets/Scenes/LoginScene.unity",
                OpenSceneMode.Additive);
            Scene arenaScene = EditorSceneManager.OpenScene(
                "Assets/Scenes/ArenaScene.unity",
                OpenSceneMode.Additive);

            try
            {
                Assert.True(SceneManager.SetActiveScene(loginScene));
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();
                PlayerClientBootstrap bootstrap = Object.FindAnyObjectByType<PlayerClientBootstrap>();

                Assert.NotNull(bootstrap);
                Assert.Null(Object.FindAnyObjectByType<PlayerNetworkSessionController>());
                Assert.Null(GameObject.Find("ArenaFloor"));
                Assert.Null(GameObject.Find("PlayerAvatar"));

                Assert.True(SceneManager.SetActiveScene(arenaScene));
                bootstrap.HandleSceneLoadedForTests(arenaScene, LoadSceneMode.Single);

                PlayerNetworkSessionController[] controllers =
                    Object.FindObjectsByType<PlayerNetworkSessionController>(
                        FindObjectsInactive.Exclude);
                Assert.AreEqual(1, controllers.Length);
                Assert.NotNull(GameObject.Find("ArenaFloor"));
                Assert.NotNull(GameObject.Find("PlayerAvatar"));
                Assert.NotNull(Camera.main);
                Assert.NotNull(Object.FindAnyObjectByType<PlayerLocalController>());
                Assert.NotNull(Object.FindAnyObjectByType<PlayerCameraFollow>());
                Assert.NotNull(Object.FindAnyObjectByType<PlayerBattleParticipantRenderer>());
                Assert.NotNull(Object.FindAnyObjectByType<PlayerDropMarkerRenderer>());
                Assert.NotNull(Object.FindAnyObjectByType<PlayerInventoryStatusRenderer>());
                AssertRetiredDebugObjectsAbsent();
            }
            finally
            {
                DestroyBootstrapRoots();
                if (previousScene.IsValid())
                {
                    SceneManager.SetActiveScene(previousScene);
                }

                EditorSceneManager.CloseScene(loginScene, true);
                EditorSceneManager.CloseScene(arenaScene, true);
            }
        }

        [Test]
        public void ArenaThenLobbySceneLoadedClearsArenaRuntimeChildren()
        {
            Scene previousScene = SceneManager.GetActiveScene();
            Scene arenaScene = EditorSceneManager.OpenScene(
                "Assets/Scenes/ArenaScene.unity",
                OpenSceneMode.Additive);
            Scene lobbyScene = EditorSceneManager.OpenScene(
                "Assets/Scenes/LobbyScene.unity",
                OpenSceneMode.Additive);

            try
            {
                Assert.True(SceneManager.SetActiveScene(arenaScene));
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();
                PlayerClientBootstrap bootstrap = Object.FindAnyObjectByType<PlayerClientBootstrap>();
                bootstrap.HandleSceneLoadedForTests(arenaScene, LoadSceneMode.Single);

                Assert.NotNull(GameObject.Find("ArenaFloor"));
                AssertRetiredDebugObjectsAbsent();

                Assert.True(SceneManager.SetActiveScene(lobbyScene));
                bootstrap.HandleSceneLoadedForTests(lobbyScene, LoadSceneMode.Single);

                Assert.NotNull(Object.FindAnyObjectByType<PlayerNetworkSessionController>());
                Assert.IsNull(GameObject.Find("ArenaFloor"));
                Assert.IsNull(GameObject.Find("PlayerAvatar"));
                AssertRetiredDebugObjectsAbsent();
            }
            finally
            {
                DestroyBootstrapRoots();
                if (previousScene.IsValid())
                {
                    SceneManager.SetActiveScene(previousScene);
                }

                EditorSceneManager.CloseScene(arenaScene, true);
                EditorSceneManager.CloseScene(lobbyScene, true);
            }
        }

        [Test]
        public void EnsureBootstrappedForRuntimeDoesNotCreateDuplicateRoot()
        {
            try
            {
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();

                PlayerClientBootstrap[] bootstraps = Object.FindObjectsByType<PlayerClientBootstrap>(
                    FindObjectsInactive.Exclude);
                Assert.AreEqual(1, bootstraps.Length);
            }
            finally
            {
                DestroyBootstrapRoots();
            }
        }

        private static void DestroyBootstrapRoots()
        {
            PlayerClientBootstrap[] bootstraps = Object.FindObjectsByType<PlayerClientBootstrap>(
                FindObjectsInactive.Include);
            foreach (PlayerClientBootstrap bootstrap in bootstraps)
            {
                Object.DestroyImmediate(bootstrap.gameObject);
            }

            GameSessionRoot[] sessionRoots = Object.FindObjectsByType<GameSessionRoot>(
                FindObjectsInactive.Include);
            foreach (GameSessionRoot sessionRoot in sessionRoots)
            {
                Object.DestroyImmediate(sessionRoot.gameObject);
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
    }
}
