using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class UnityAssemblyBoundaryTests
    {
        [Test]
        public void RepositorySourcesRespectPlayerFlowAssemblyBoundaries()
        {
            var violations = new List<string>();
            string root = Path.Combine(Application.dataPath, "LootOfLegends");

            string presentationAssembly = File.ReadAllText(Path.Combine(
                root,
                "Presentation/LootOfLegends.Presentation.asmdef"));
            AddAssemblyReferenceViolation(
                violations,
                "Presentation",
                presentationAssembly,
                "LootOfLegends.Protocol",
                "LootOfLegends.Transport");
            foreach (string source in ReadSources(root, "Presentation"))
            {
                AddPresentationSourceViolation(violations, source);
            }

            string battleAssembly = File.ReadAllText(Path.Combine(
                root,
                "Battle/LootOfLegends.Battle.asmdef"));
            AddAssemblyReferenceViolation(
                violations,
                "Battle",
                battleAssembly,
                "LootOfLegends.LobbyRoom");

            foreach (string capability in new[] { "Battle", "LobbyRoom", "Collection", "Session" })
            {
                foreach (string source in ReadSources(root, capability))
                {
                    AddCommandReceiveViolation(violations, capability, source);
                }
            }

            foreach (string source in ReadSources(root, "Bootstrap"))
            {
                AddBootstrapPolicyViolation(violations, source);
            }

            Assert.That(violations, Is.Empty, string.Join(Environment.NewLine, violations));
        }

        [Test]
        public void PresentationPolicyRejectsRawSocketAndProtocolCodec()
        {
            var violations = new List<string>();
            AddPresentationSourceViolation(
                violations,
                "using System.Net.Sockets; BattleLoadProtocolCodec.DecodeServerFrame(frame);");

            Assert.That(violations, Has.Count.EqualTo(2));
        }

        [Test]
        public void BattlePolicyRejectsLobbyRoomAssemblyReference()
        {
            var violations = new List<string>();
            AddAssemblyReferenceViolation(
                violations,
                "Battle",
                "{ \"references\": [\"LootOfLegends.LobbyRoom\"] }",
                "LootOfLegends.LobbyRoom");

            Assert.That(violations, Has.Count.EqualTo(1));
        }

        [Test]
        public void CommandPolicyRejectsReceiveOwnership()
        {
            var violations = new List<string>();
            AddCommandReceiveViolation(
                violations,
                "Battle",
                "public Task SendAsync() { return socket.ReceiveAsync(); }");

            Assert.That(violations, Has.Count.EqualTo(1));
        }

        [Test]
        public void BootstrapPolicyRejectsGameplayBranch()
        {
            var violations = new List<string>();
            AddBootstrapPolicyViolation(
                violations,
                "if (result.Outcome == FinalResultOutcome.MonsterDefeated) { }");

            Assert.That(violations, Has.Count.EqualTo(2));
        }

        private static IEnumerable<string> ReadSources(string root, string relativeDirectory)
        {
            string directory = Path.Combine(root, relativeDirectory);
            if (!Directory.Exists(directory))
            {
                return Array.Empty<string>();
            }
            return Directory.EnumerateFiles(directory, "*.cs", SearchOption.AllDirectories)
                .Select(File.ReadAllText);
        }

        private static void AddAssemblyReferenceViolation(
            ICollection<string> violations,
            string owner,
            string assemblyDefinition,
            params string[] forbiddenReferences)
        {
            foreach (string forbidden in forbiddenReferences)
            {
                if (assemblyDefinition.Contains($"\"{forbidden}\""))
                {
                    violations.Add($"{owner} assembly must not reference {forbidden}");
                }
            }
        }

        private static void AddPresentationSourceViolation(
            ICollection<string> violations,
            string source)
        {
            if (source.Contains("System.Net.Sockets") ||
                source.Contains("TcpClient") ||
                source.Contains("UdpClient"))
            {
                violations.Add("Presentation must not own a raw socket");
            }
            if (source.Contains("ProtocolCodec"))
            {
                violations.Add("Presentation must not call a protocol codec");
            }
        }

        private static void AddCommandReceiveViolation(
            ICollection<string> violations,
            string owner,
            string source)
        {
            if (source.Contains("ReceiveAsync("))
            {
                violations.Add($"{owner} command/capability source must not receive from a socket");
            }
        }

        private static void AddBootstrapPolicyViolation(
            ICollection<string> violations,
            string source)
        {
            foreach (string forbidden in new[]
                     {
                         "FinalResultOutcome",
                         "MonsterDefeated",
                         "CombatTimeout",
                         "ClaimLoot",
                         "AttackIntent",
                         "MoveIntent"
                     })
            {
                if (source.Contains(forbidden))
                {
                    violations.Add($"Bootstrap must not branch on gameplay policy: {forbidden}");
                }
            }
        }
    }
}
