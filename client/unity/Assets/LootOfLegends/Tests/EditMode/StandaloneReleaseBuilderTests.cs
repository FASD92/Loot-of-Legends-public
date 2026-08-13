using System.IO;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class StandaloneReleaseBuilderTests
    {
        [Test]
        public void BuilderPinsReleasePlatformsWithoutDevelopmentFlags()
        {
            string source = File.ReadAllText(Path.Combine(
                Application.dataPath,
                "LootOfLegends/Editor/StandaloneReleaseBuilder.cs"));

            StringAssert.Contains("6000.3.21f1", source);
            StringAssert.Contains("ScriptingImplementation.IL2CPP", source);
            StringAssert.Contains("Il2CppCompilerConfiguration.Release", source);
            StringAssert.Contains("BuildTarget.StandaloneWindows64", source);
            StringAssert.Contains("OSArchitecture.x64", source);
            StringAssert.Contains("OSArchitecture.x64ARM64", source);
            StringAssert.Contains("BuildOptions.None", source);
            StringAssert.DoesNotContain("BuildOptions.Development", source);
            StringAssert.DoesNotContain("AllowDebugging", source);
            StringAssert.DoesNotContain("ConnectWithProfiler", source);
        }

        [Test]
        public void BuilderWritesBoundedReleaseIdentity()
        {
            string source = File.ReadAllText(Path.Combine(
                Application.dataPath,
                "LootOfLegends/Editor/StandaloneReleaseBuilder.cs"));

            StringAssert.Contains("sourceSha", source);
            StringAssert.Contains("rcTag", source);
            StringAssert.Contains("contractDigest", source);
            StringAssert.Contains("developmentBuild = false", source);
            StringAssert.Contains("debugGameplayMutation = false", source);
            StringAssert.Contains("RAW_RESTRICTED_SANITIZED_REQUIRED", source);
            StringAssert.Contains("codeSigning = \"NOT_PERFORMED\"", source);
            StringAssert.Contains("notarization = \"NOT_PERFORMED\"", source);
        }

        [Test]
        public void BuildScriptPinsSourcePackagesAndChecksumsZip()
        {
            string repository = Path.GetFullPath(Path.Combine(Application.dataPath, "../../.."));
            string script = File.ReadAllText(Path.Combine(
                repository,
                "scripts/release/build_unity.sh"));

            StringAssert.Contains("git status --porcelain", script);
            StringAssert.Contains("refs/tags/$rc_tag^{commit}", script);
            StringAssert.Contains("ProjectVersion.txt", script);
            StringAssert.Contains("Packages/manifest.json", script);
            StringAssert.Contains("Packages/packages-lock.json", script);
            StringAssert.Contains("StandaloneReleaseBuilder.BuildMac", script);
            StringAssert.Contains("StandaloneReleaseBuilder.BuildWindows", script);
            StringAssert.Contains(".zip", script);
            StringAssert.Contains(".sha256", script);
            StringAssert.DoesNotContain("codesign", script);
            StringAssert.DoesNotContain("notarytool", script);
        }
    }
}
