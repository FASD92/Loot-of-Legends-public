using System.Linq;
using LootOfLegends.Presentation.Collection;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Presentation.FinalResult;
using LootOfLegends.Presentation.Lobby;
using LootOfLegends.Presentation.Room;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class PlayerFlowSceneAssetTests
    {
        [TestCase("Assets/Scenes/LobbyScene.unity", typeof(LobbyTextView))]
        [TestCase("Assets/Scenes/RoomScene.unity", typeof(RoomTextView))]
        [TestCase("Assets/Scenes/ArenaScene.unity", typeof(ArenaTextView))]
        public void PresentationSceneLoadsWithoutMissingScripts(string path, System.Type viewType)
        {
            Scene scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Additive);
            try
            {
                GameObject[] roots = scene.GetRootGameObjects();
                Assert.That(roots, Is.Not.Empty);
                Assert.That(
                    roots.SelectMany(root => root.GetComponentsInChildren(viewType, true)),
                    Is.Not.Empty);
                Assert.That(
                    roots.Sum(MissingScriptCount),
                    Is.Zero,
                    path + " contains a missing MonoBehaviour script");
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void ArenaSceneContainsFinalResultOverlay()
        {
            Scene scene = EditorSceneManager.OpenScene(
                "Assets/Scenes/ArenaScene.unity",
                OpenSceneMode.Additive);
            try
            {
                Assert.That(
                    scene.GetRootGameObjects()
                        .SelectMany(root =>
                            root.GetComponentsInChildren<FinalResultTextView>(true)),
                    Is.Not.Empty);
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        [Test]
        public void LobbySceneContainsCollectionView()
        {
            Scene scene = EditorSceneManager.OpenScene(
                "Assets/Scenes/LobbyScene.unity",
                OpenSceneMode.Additive);
            try
            {
                Assert.That(
                    scene.GetRootGameObjects()
                        .SelectMany(root =>
                            root.GetComponentsInChildren<CollectionTextView>(true)),
                    Is.Not.Empty);
            }
            finally
            {
                EditorSceneManager.CloseScene(scene, true);
            }
        }

        private static int MissingScriptCount(GameObject root)
        {
            int count = 0;
            foreach (Transform child in root.GetComponentsInChildren<Transform>(true))
            {
                count += GameObjectUtility.GetMonoBehavioursWithMissingScriptCount(
                    child.gameObject);
            }
            return count;
        }
    }
}
