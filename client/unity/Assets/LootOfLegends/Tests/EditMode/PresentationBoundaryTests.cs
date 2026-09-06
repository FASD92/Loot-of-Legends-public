using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class PresentationBoundaryTests
    {
        [Test]
        public void RepositorySourcesRespectPresentationResponsibilities()
        {
            var violations = new List<string>();
            string root = Path.Combine(Application.dataPath, "LootOfLegends");

            foreach (string path in Directory.EnumerateFiles(
                         Path.Combine(root, "Presentation"),
                         "*.cs",
                         SearchOption.AllDirectories))
            {
                string source = File.ReadAllText(path);
                UnityAssemblyBoundaryTests.AddPresentationSourceViolation(
                    violations,
                    source);
                if (path.EndsWith("Presenter.cs", StringComparison.Ordinal) &&
                    !source.Contains(": MonoBehaviour"))
                {
                    AddScreenPresenterViolation(violations, source);
                }
                if (path.EndsWith("View.cs", StringComparison.Ordinal))
                {
                    AddViewAdapterViolation(violations, source);
                }
                if (path.EndsWith(
                        "DevelopmentPlayerFlowDriver.cs",
                        StringComparison.Ordinal))
                {
                    AddCompositionUiPolicyViolation(
                        violations,
                        "DevelopmentPlayerFlowDriver",
                        source);
                }
            }

            foreach (string path in Directory.EnumerateFiles(
                         Path.Combine(root, "Bootstrap"),
                         "*.cs",
                         SearchOption.AllDirectories))
            {
                AddCompositionUiPolicyViolation(
                    violations,
                    "Bootstrap",
                    File.ReadAllText(path));
            }

            Assert.That(violations, Is.Empty, string.Join(Environment.NewLine, violations));
        }

        [Test]
        public void ScreenPresenterPolicyRejectsSceneAndAssetLookup()
        {
            var violations = new List<string>();
            AddScreenPresenterViolation(
                violations,
                "SceneManager.LoadScene(\"RoomScene\"); " +
                "Resources.Load(\"room.panel\");");

            Assert.That(violations, Has.Count.EqualTo(2));
        }

        [Test]
        public void ViewAdapterPolicyRejectsCapabilityOwners()
        {
            var violations = new List<string>();
            AddViewAdapterViolation(
                violations,
                "private LobbyRoomReadModel readModel; " +
                "private ILobbyRoomCommands commands;");

            Assert.That(violations, Has.Count.EqualTo(2));
        }

        [Test]
        public void CompositionPolicyRejectsScreenAndWidgetOwnership()
        {
            var violations = new List<string>();
            AddCompositionUiPolicyViolation(
                violations,
                "Bootstrap",
                "using UnityEngine.UI; " +
                "[SerializeField] private LobbyScreenView lobby;");

            Assert.That(violations, Has.Count.EqualTo(3));
        }

        [Test]
        public void ProductViewsDoNotUseImmediateModeOrTextMesh()
        {
            string root = Path.Combine(Application.dataPath, "LootOfLegends/Presentation");
            var paths = Directory.EnumerateFiles(
                    root,
                    "*ScreenView.cs",
                    SearchOption.AllDirectories)
                .Concat(new[]
                {
                    Path.Combine(root, "Common/SafeFailureTextView.cs")
                });

            foreach (string path in paths)
            {
                string source = File.ReadAllText(path);
                StringAssert.DoesNotContain("OnGUI(", source, path);
                StringAssert.DoesNotContain("TextMesh", source, path);
            }
        }

        [Test]
        public void FinalResultViewDoesNotDependOnArenaExitFlow()
        {
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/FinalResult/FinalResultScreenView.cs");
            string source = File.ReadAllText(path);

            StringAssert.DoesNotContain("LootOfLegends.Presentation.Arena", source);
            StringAssert.DoesNotContain("ArenaExitPresenter", source);
            StringAssert.DoesNotContain("IArenaExitView", source);
        }

        private static void AddScreenPresenterViolation(
            ICollection<string> violations,
            string source)
        {
            foreach (string forbidden in new[]
                     {
                         "SceneManager",
                         "FindFirstObjectByType",
                         "Resources.Load",
                         "AssetDatabase",
                         "PresentationCatalog"
                     })
            {
                if (source.Contains(forbidden))
                {
                    violations.Add($"Screen Presenter must not own Unity lookup: {forbidden}");
                }
            }
        }

        private static void AddViewAdapterViolation(
            ICollection<string> violations,
            string source)
        {
            foreach (string forbidden in new[]
                     {
                         "PlayerSessionReadModel",
                         "LobbyRoomReadModel",
                         "BattleLoadReadModel",
                         "BattleResultReadModel",
                         "CollectionReadModel",
                         "ArenaPlayerFlowReadModel",
                         "ILobbyRoomCommands",
                         "BattleLoadCoordinator",
                         "ArenaInputFacade",
                         "ICollectionApi"
                     })
            {
                if (source.Contains(forbidden))
                {
                    violations.Add($"View adapter must use its Presenter: {forbidden}");
                }
            }
        }

        private static void AddCompositionUiPolicyViolation(
            ICollection<string> violations,
            string owner,
            string source)
        {
            foreach (string forbidden in new[]
                     {
                         "ScreenView",
                         "PresentationCatalog",
                         "UnityEngine.UI",
                         "UnityEngine.UIElements",
                         "TMPro",
                         "[SerializeField]",
                         "SceneManager"
                     })
            {
                if (source.Contains(forbidden))
                {
                    violations.Add($"{owner} must not own screen UI policy: {forbidden}");
                }
            }
        }
    }
}
