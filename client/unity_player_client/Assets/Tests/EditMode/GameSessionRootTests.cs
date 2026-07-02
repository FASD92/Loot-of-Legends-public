using System;
using System.IO;
using System.Reflection;
using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityObject = UnityEngine.Object;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class GameSessionRootTests
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
        public void RootStoresMetaAdmissionAndGameSessionToken()
        {
            GameObject rootObject = new GameObject("GameSessionRoot");
            try
            {
                GameSessionRoot root = rootObject.AddComponent<GameSessionRoot>();
                Assert.True(PlayerServerEndpoint.TryCreate(
                    "game.example.com",
                    41000,
                    41001,
                    out PlayerServerEndpoint endpoint));

                root.StoreAdmission(new MetaAdmissionResult(
                    "session-token-1",
                    endpoint,
                    1_785_000_000_000L));

                Assert.True(root.HasAdmission);
                Assert.AreEqual("session-token-1", root.GameSessionToken);
                Assert.AreEqual("game.example.com", root.GameServerEndpoint.Host);
                Assert.AreEqual(41000, root.GameServerEndpoint.TcpPort);
                Assert.AreEqual(41001, root.GameServerEndpoint.RudpPort);
                Assert.AreEqual(1_785_000_000_000L, root.ReservationExpiresAt);
            }
            finally
            {
                UnityObject.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void RootKeepsSessionReplacedMessageDistinctFromGenericDisconnect()
        {
            GameObject rootObject = new GameObject("GameSessionRoot");
            try
            {
                GameSessionRoot root = rootObject.AddComponent<GameSessionRoot>();

                root.MarkDisconnected();
                Assert.AreEqual(GameSessionRoot.GenericDisconnectMessage, root.LastDisconnectMessage);

                root.MarkSessionReplaced();
                Assert.AreEqual(
                    "다른 클라이언트에서 접속되어 연결이 종료되었습니다",
                    root.LastDisconnectMessage);
                Assert.AreNotEqual(
                    GameSessionRoot.GenericDisconnectMessage,
                    root.LastDisconnectMessage);
            }
            finally
            {
                UnityObject.DestroyImmediate(rootObject);
            }
        }

        [Test]
        public void BuildSettingsStartsFromLoginScene()
        {
            string buildSettingsPath = Path.GetFullPath(Path.Combine(
                Application.dataPath,
                "../ProjectSettings/EditorBuildSettings.asset"));
            string yaml = File.ReadAllText(buildSettingsPath);

            Assert.AreEqual(
                "Assets/Scenes/LoginScene.unity",
                FirstEnabledBuildScenePath(yaml));
        }

        [Test]
        public void DuplicateRootsPreserveExistingOwnerAndRemoveSecondOwner()
        {
            GameObject duplicateObject = null;
            try
            {
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();
                GameSessionRoot owner = UnityObject.FindAnyObjectByType<GameSessionRoot>();
                InvokeAwake(owner);
                Assert.NotNull(owner);
                Assert.AreSame(owner, GameSessionRoot.Instance);

                duplicateObject = new GameObject("DuplicateGameSessionRoot");
                GameSessionRoot duplicate = duplicateObject.AddComponent<GameSessionRoot>();
                InvokeAwake(duplicate);

                GameSessionRoot[] roots = UnityObject.FindObjectsByType<GameSessionRoot>(
                    FindObjectsInactive.Include);
                Assert.AreEqual(1, roots.Length);
                Assert.AreSame(owner, GameSessionRoot.Instance);
                Assert.AreSame(owner, roots[0]);
                Assert.True(duplicate == null || !duplicate || !duplicate.enabled);
            }
            finally
            {
                if (duplicateObject != null)
                {
                    UnityObject.DestroyImmediate(duplicateObject);
                }

                DestroyBootstrapRoots();
            }
        }

        [Test]
        public void BootstrapCreatesGameSessionRootWithoutDuplicatingBootstrapBehavior()
        {
            try
            {
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();
                PlayerClientBootstrap.EnsureBootstrappedForRuntime();

                PlayerClientBootstrap[] bootstraps = UnityObject.FindObjectsByType<PlayerClientBootstrap>(
                    FindObjectsInactive.Exclude);
                GameSessionRoot[] sessionRoots = UnityObject.FindObjectsByType<GameSessionRoot>(
                    FindObjectsInactive.Exclude);

                Assert.AreEqual(1, bootstraps.Length);
                Assert.AreEqual(1, sessionRoots.Length);
                Assert.AreSame(bootstraps[0].gameObject, sessionRoots[0].gameObject);
                Assert.NotNull(UnityObject.FindAnyObjectByType<PlayerNetworkSessionController>());
                AssertRetiredDebugObjectsAbsent();
                Assert.Null(GameObject.Find("ArenaFloor"));
                Assert.Null(GameObject.Find("PlayerAvatar"));
            }
            finally
            {
                DestroyBootstrapRoots();
            }
        }

        [Test]
        public void LoginSceneBootstrapDoesNotCreateArenaOrManualControlsBeforeAdmission()
        {
            Scene previousScene = SceneManager.GetActiveScene();
            Scene loginScene = EditorSceneManager.OpenScene(
                "Assets/Scenes/LoginScene.unity",
                OpenSceneMode.Additive);

            try
            {
                Assert.True(SceneManager.SetActiveScene(loginScene));

                PlayerClientBootstrap.EnsureBootstrappedForRuntime();

                Assert.NotNull(UnityObject.FindAnyObjectByType<PlayerClientBootstrap>());
                Assert.NotNull(UnityObject.FindAnyObjectByType<GameSessionRoot>());
                Assert.Null(GameObject.Find("ArenaFloor"));
                Assert.Null(UnityObject.FindAnyObjectByType<PlayerNetworkSessionController>());
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
            }
        }

        private static void DestroyBootstrapRoots()
        {
            PlayerClientBootstrap[] bootstraps = UnityObject.FindObjectsByType<PlayerClientBootstrap>(
                FindObjectsInactive.Exclude);
            foreach (PlayerClientBootstrap bootstrap in bootstraps)
            {
                UnityObject.DestroyImmediate(bootstrap.gameObject);
            }

            GameSessionRoot[] sessionRoots = UnityObject.FindObjectsByType<GameSessionRoot>(
                FindObjectsInactive.Include);
            foreach (GameSessionRoot sessionRoot in sessionRoots)
            {
                UnityObject.DestroyImmediate(sessionRoot.gameObject);
            }
        }

        private static void AssertRetiredDebugObjectsAbsent()
        {
            Assert.Null(GameObject.Find("InventoryActionPreview"));
            Assert.Null(GameObject.Find("ManualCommandPanel"));
            Assert.Null(GameObject.Find("ManualBattleCommandPanel"));
            Assert.Null(GameObject.Find("ManualCombatCommandPanel"));
            Assert.Null(GameObject.Find("ManualLootCommandPanel"));
            Assert.Null(GameObject.Find("ManualSmokeChecklist"));
        }

        private static string FirstEnabledBuildScenePath(string yaml)
        {
            using (StringReader reader = new StringReader(yaml))
            {
                string line;
                while ((line = reader.ReadLine()) != null)
                {
                    if (!line.Trim().Equals("- enabled: 1", StringComparison.Ordinal))
                    {
                        continue;
                    }

                    while ((line = reader.ReadLine()) != null)
                    {
                        string trimmed = line.Trim();
                        if (trimmed.StartsWith("path: ", StringComparison.Ordinal))
                        {
                            return trimmed.Substring("path: ".Length);
                        }

                        if (trimmed.StartsWith("- enabled: ", StringComparison.Ordinal))
                        {
                            break;
                        }
                    }
                }
            }

            return string.Empty;
        }

        private static void InvokeAwake(GameSessionRoot root)
        {
            Assert.NotNull(root);
            MethodInfo awake = typeof(GameSessionRoot).GetMethod(
                "Awake",
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.NotNull(awake);
            awake.Invoke(root, null);
        }
    }
}
