using LootOfLegends.Editor;
using NUnit.Framework;
using UnityEditor;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class Release0StandaloneBuildTests
    {
        [Test]
        public void Release0SceneListExcludesDebugSampleScene()
        {
            CollectionAssert.AreEqual(
                new[]
                {
                    "Assets/Scenes/LoginScene.unity",
                    "Assets/Scenes/LobbyScene.unity",
                    "Assets/Scenes/RoomScene.unity",
                    "Assets/Scenes/ArenaScene.unity"
                },
                Release0StandaloneBuild.Release0ScenePaths);
            CollectionAssert.DoesNotContain(
                Release0StandaloneBuild.Release0ScenePaths,
                "Assets/Scenes/SampleScene.unity");
        }

        [Test]
        public void WindowsBuildUsesRelease0OutputPath()
        {
            BuildPlayerOptions options =
                Release0StandaloneBuild.CreateBuildPlayerOptions(
                    BuildTarget.StandaloneWindows64);

            Assert.AreEqual(
                "Builds/Release0/Windows/LootOfLegendsRelease0.exe",
                options.locationPathName);
        }

        [Test]
        public void MacOSBuildUsesRelease0OutputPath()
        {
            BuildPlayerOptions options =
                Release0StandaloneBuild.CreateBuildPlayerOptions(
                    BuildTarget.StandaloneOSX);

            Assert.AreEqual(
                "Builds/Release0/macOS/LootOfLegendsRelease0.app",
                options.locationPathName);
        }

        [Test]
        public void DefaultBuildOptionsAreNonDevelopment()
        {
            BuildPlayerOptions options =
                Release0StandaloneBuild.CreateBuildPlayerOptions(
                    BuildTarget.StandaloneWindows64);

            Assert.AreEqual(BuildOptions.None, options.options & BuildOptions.Development);
        }

        [Test]
        public void ReleaseBuildCleansPlayerBuildCache()
        {
            BuildPlayerOptions options =
                Release0StandaloneBuild.CreateBuildPlayerOptions(
                    BuildTarget.StandaloneWindows64);

            Assert.AreEqual(
                BuildOptions.CleanBuildCache,
                options.options & BuildOptions.CleanBuildCache);
        }
    }
}
