using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace LootOfLegends.Editor
{
    public static class StandaloneReleaseBuilder
    {
        private const string RequiredUnityVersion = "6000.3.21f1";

        private static readonly string[] RequiredScenes =
        {
            "Assets/Scenes/LoginScene.unity",
            "Assets/Scenes/LobbyScene.unity",
            "Assets/Scenes/RoomScene.unity",
            "Assets/Scenes/ArenaScene.unity"
        };

        public static void BuildMac()
        {
            Build(
                BuildTarget.StandaloneOSX,
                OSArchitecture.x64ARM64,
                ".app",
                "macOS Universal");
        }

        public static void BuildWindows()
        {
            Build(
                BuildTarget.StandaloneWindows64,
                OSArchitecture.x64,
                ".exe",
                "Windows x86_64");
        }

        private static void Build(
            BuildTarget target,
            OSArchitecture architecture,
            string requiredExtension,
            string architectureName)
        {
            if (Application.unityVersion != RequiredUnityVersion)
            {
                throw new InvalidOperationException("Unity editor version is not pinned");
            }

            string output = RequiredPath("--loot-build-output=", requiredExtension);
            string manifest = RequiredPath("--loot-release-manifest=", ".json");
            string sourceSha = RequiredMatch("--loot-source-sha=", "^[0-9a-f]{40}$");
            string rcTag = RequiredMatch(
                "--loot-rc-tag=",
                "^v1\\.0\\.0-rc\\.[1-9][0-9]*$");
            string contractDigest = RequiredMatch(
                "--loot-contract-digest=",
                "^[0-9a-f]{64}$");
            string[] scenes = EditorBuildSettings.scenes
                .Where(scene => scene.enabled)
                .Select(scene => scene.path)
                .ToArray();
            if (!scenes.SequenceEqual(RequiredScenes))
            {
                throw new InvalidOperationException("Release build scene contract is not configured");
            }

            Directory.CreateDirectory(Path.GetDirectoryName(output));
            Directory.CreateDirectory(Path.GetDirectoryName(manifest));

            NamedBuildTarget standalone = NamedBuildTarget.Standalone;
            ScriptingImplementation previousBackend =
                PlayerSettings.GetScriptingBackend(standalone);
            Il2CppCompilerConfiguration previousCompiler =
                PlayerSettings.GetIl2CppCompilerConfiguration(standalone);
            int previousArchitecture = PlayerSettings.GetArchitecture(standalone);
            string previousMacArchitecture = EditorUserBuildSettings.GetPlatformSettings(
                "OSXUniversal",
                "Architecture");

            try
            {
                PlayerSettings.SetScriptingBackend(
                    standalone,
                    ScriptingImplementation.IL2CPP);
                PlayerSettings.SetIl2CppCompilerConfiguration(
                    standalone,
                    Il2CppCompilerConfiguration.Release);
                PlayerSettings.SetArchitecture(standalone, (int)architecture);
                if (target == BuildTarget.StandaloneOSX)
                {
                    EditorUserBuildSettings.SetPlatformSettings(
                        "OSXUniversal",
                        "Architecture",
                        OSArchitecture.x64ARM64.ToString());
                }

                BuildReport report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
                {
                    scenes = scenes,
                    locationPathName = output,
                    target = target,
                    options = BuildOptions.None
                });
                if (report.summary.result != BuildResult.Succeeded)
                {
                    throw new InvalidOperationException("Standalone release build failed");
                }

                var identity = new ReleaseIdentity
                {
                    schemaVersion = "1.0.0",
                    product = "LootOfLegendsV2",
                    rcTag = rcTag,
                    sourceSha = sourceSha,
                    contractDigest = contractDigest,
                    unityVersion = Application.unityVersion,
                    buildTarget = target.ToString(),
                    architecture = architectureName,
                    scriptingBackend = "IL2CPP",
                    developmentBuild = false,
                    debugGameplayMutation = false,
                    codeSigning = "NOT_PERFORMED",
                    notarization = "NOT_PERFORMED",
                    playerLogPolicy = "RAW_RESTRICTED_SANITIZED_REQUIRED"
                };
                File.WriteAllText(
                    manifest,
                    JsonUtility.ToJson(identity, true) + Environment.NewLine,
                    new UTF8Encoding(false));
            }
            finally
            {
                PlayerSettings.SetScriptingBackend(standalone, previousBackend);
                PlayerSettings.SetIl2CppCompilerConfiguration(standalone, previousCompiler);
                PlayerSettings.SetArchitecture(standalone, previousArchitecture);
                if (target == BuildTarget.StandaloneOSX)
                {
                    EditorUserBuildSettings.SetPlatformSettings(
                        "OSXUniversal",
                        "Architecture",
                        previousMacArchitecture);
                }
            }
        }

        private static string RequiredPath(string prefix, string extension)
        {
            string value = RequiredArgument(prefix);
            if (!Path.IsPathRooted(value) ||
                !value.EndsWith(extension, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(prefix + " must be an absolute " + extension + " path");
            }
            return value;
        }

        private static string RequiredMatch(string prefix, string pattern)
        {
            string value = RequiredArgument(prefix);
            if (!Regex.IsMatch(value, pattern, RegexOptions.CultureInvariant))
            {
                throw new InvalidOperationException(prefix + " is invalid");
            }
            return value;
        }

        private static string RequiredArgument(string prefix)
        {
            string value = Environment.GetCommandLineArgs().FirstOrDefault(
                argument => argument.StartsWith(prefix, StringComparison.Ordinal));
            if (value == null || value.Length == prefix.Length)
            {
                throw new InvalidOperationException(prefix + " is required");
            }
            return value.Substring(prefix.Length);
        }

        [Serializable]
        private sealed class ReleaseIdentity
        {
            public string schemaVersion;
            public string product;
            public string rcTag;
            public string sourceSha;
            public string contractDigest;
            public string unityVersion;
            public string buildTarget;
            public string architecture;
            public string scriptingBackend;
            public bool developmentBuild;
            public bool debugGameplayMutation;
            public string codeSigning;
            public string notarization;
            public string playerLogPolicy;
        }
    }
}
