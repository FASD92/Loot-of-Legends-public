using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;

namespace LootOfLegends.Editor
{
    public static class StandaloneDevelopmentBuilder
    {
        public static void BuildMac()
        {
            PlayerSettings.SetScriptingBackend(
                NamedBuildTarget.Standalone,
                ScriptingImplementation.Mono2x);
            SetMacArchitecture(OSArchitecture.ARM64);
            Build(BuildTarget.StandaloneOSX, ".app");
        }

        private static void SetMacArchitecture(OSArchitecture architecture)
        {
            PlayerSettings.SetArchitecture(
                NamedBuildTarget.Standalone,
                (int)architecture);
            EditorUserBuildSettings.SetPlatformSettings(
                "OSXUniversal",
                "Architecture",
                architecture.ToString());
        }

        private static void Build(BuildTarget target, string requiredExtension)
        {
            string output = Argument("--loot-build-output=");
            if (string.IsNullOrWhiteSpace(output) || !Path.IsPathRooted(output) ||
                !output.EndsWith(requiredExtension, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(
                    "Standalone build output must be an absolute target path");
            }
            string parent = Path.GetDirectoryName(output);
            if (string.IsNullOrEmpty(parent))
            {
                throw new InvalidOperationException("Standalone build parent is missing");
            }
            Directory.CreateDirectory(parent);
            string[] scenes = EditorBuildSettings.scenes
                .Where(scene => scene.enabled)
                .Select(scene => scene.path)
                .ToArray();
            if (scenes.Length != 4 ||
                scenes[0] != "Assets/Scenes/LoginScene.unity")
            {
                throw new InvalidOperationException(
                    "Standalone build scene contract is not configured");
            }

            BuildReport report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
            {
                scenes = scenes,
                locationPathName = output,
                target = target,
                options = BuildOptions.Development
            });
            if (report.summary.result != BuildResult.Succeeded)
            {
                throw new InvalidOperationException("Standalone development build failed");
            }
        }

        private static string Argument(string prefix)
        {
            string value = Environment.GetCommandLineArgs().FirstOrDefault(
                argument => argument.StartsWith(prefix, StringComparison.Ordinal));
            return value == null ? null : value.Substring(prefix.Length);
        }
    }
}
