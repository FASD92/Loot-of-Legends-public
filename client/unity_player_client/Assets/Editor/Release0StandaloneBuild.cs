#if UNITY_EDITOR
using System;
using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;

namespace LootOfLegends.Editor
{
    public static class Release0StandaloneBuild
    {
        public const string DevelopmentBuildEnvironmentVariable =
            "LOL_RELEASE0_DEVELOPMENT_BUILD";
        public const string Windows64OutputPath =
            "Builds/Release0/Windows/LootOfLegendsRelease0.exe";
        public const string MacOSOutputPath =
            "Builds/Release0/macOS/LootOfLegendsRelease0.app";

        private static readonly string[] Release0Scenes =
        {
            "Assets/Scenes/LoginScene.unity",
            "Assets/Scenes/LobbyScene.unity",
            "Assets/Scenes/RoomScene.unity",
            "Assets/Scenes/ArenaScene.unity"
        };

        public static string[] Release0ScenePaths =>
            (string[])Release0Scenes.Clone();

        [MenuItem("Tools/Loot of Legends/Release 0/Build Windows x64")]
        public static void BuildWindows64()
        {
            Build(BuildTarget.StandaloneWindows64);
        }

        [MenuItem("Tools/Loot of Legends/Release 0/Build macOS")]
        public static void BuildMacOS()
        {
            Build(BuildTarget.StandaloneOSX);
        }

        public static BuildPlayerOptions CreateBuildPlayerOptions(BuildTarget target)
        {
            return new BuildPlayerOptions
            {
                scenes = Release0ScenePaths,
                target = target,
                targetGroup = BuildTargetGroup.Standalone,
                locationPathName = OutputPathFor(target),
                options = BuildOptionsForEnvironment()
            };
        }

        public static string OutputPathFor(BuildTarget target)
        {
            switch (target)
            {
                case BuildTarget.StandaloneWindows64:
                    return Windows64OutputPath;
                case BuildTarget.StandaloneOSX:
                    return MacOSOutputPath;
                default:
                    throw new ArgumentOutOfRangeException(
                        nameof(target),
                        target,
                        "Release 0 supports only Windows x64 and macOS Standalone builds.");
            }
        }

        public static BuildOptions BuildOptionsForEnvironment()
        {
            BuildOptions options = BuildOptions.CleanBuildCache;
            return IsDevelopmentBuildEnabled() ?
                options |
                BuildOptions.Development :
                options;
        }

        private static void Build(BuildTarget target)
        {
            BuildPlayerOptions options = CreateBuildPlayerOptions(target);
            string directory = Path.GetDirectoryName(options.locationPathName);
            if (!string.IsNullOrEmpty(directory))
            {
                Directory.CreateDirectory(directory);
            }

            BuildReport report = BuildPipeline.BuildPlayer(options);
            if (report.summary.result != BuildResult.Succeeded)
            {
                throw new BuildFailedException(
                    $"Release 0 Standalone build failed: {report.summary.result}");
            }
        }

        private static bool IsDevelopmentBuildEnabled()
        {
            return string.Equals(
                Environment.GetEnvironmentVariable(
                    DevelopmentBuildEnvironmentVariable),
                "1",
                StringComparison.Ordinal);
        }
    }
}
#endif
