using System.IO;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class UguiPresentationContractTests
    {
        [Test]
        public void UguiPackageAndAssemblyReferencesArePinned()
        {
            string project = Directory.GetParent(Application.dataPath).FullName;
            Assert.That(
                File.ReadAllText(Path.Combine(project, "Packages/manifest.json")),
                Does.Contain("\"com.unity.ugui\": \"2.0.0\""));
            Assert.That(
                File.ReadAllText(Path.Combine(project, "Packages/packages-lock.json")),
                Does.Contain("\"com.unity.ugui\""));

            string[] assemblyDefinitions =
            {
                "LootOfLegends/Presentation/LootOfLegends.Presentation.asmdef",
                "LootOfLegends/Tests/EditMode/LootOfLegends.Tests.EditMode.asmdef",
                "LootOfLegends/Tests/PlayMode/LootOfLegends.Tests.PlayMode.asmdef"
            };
            foreach (string relativePath in assemblyDefinitions)
            {
                Assert.That(
                    File.ReadAllText(Path.Combine(Application.dataPath, relativePath)),
                    Does.Contain("\"UnityEngine.UI\""),
                    relativePath);
            }
        }
    }
}
