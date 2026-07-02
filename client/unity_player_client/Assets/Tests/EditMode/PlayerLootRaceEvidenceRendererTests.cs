using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerLootRaceEvidenceRendererTests
    {
        [Test]
        public void RenderEvidenceShowsPassWhenServerResultsMatchSameDrop()
        {
            GameObject root = new GameObject("LootRaceEvidence");
            try
            {
                PlayerLootRaceEvidenceRenderer renderer =
                    root.AddComponent<PlayerLootRaceEvidenceRenderer>();

                bool passed = renderer.RenderEvidence(
                    new PlayerLootResolved(17U, 7U, 22UL, 1001U, 1),
                    new PlayerLootRejected(
                        17U,
                        7U,
                        PlayerLootRejectReason.AlreadyClaimed),
                    new PlayerInventorySnapshot(
                        22UL,
                        1,
                        10,
                        new[] { new PlayerInventoryEntry(1001U, 1) }));

                Assert.IsTrue(passed);
                Assert.AreEqual(
                    "Loot Race PASS\nDrop 7\nWinner 22\nLoser: AlreadyClaimed\nInventory: Item 1001 x1",
                    renderer.Text);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderEvidenceShowsPendingWhenRejectedDropDoesNotMatch()
        {
            GameObject root = new GameObject("LootRaceEvidence");
            try
            {
                PlayerLootRaceEvidenceRenderer renderer =
                    root.AddComponent<PlayerLootRaceEvidenceRenderer>();

                bool passed = renderer.RenderEvidence(
                    new PlayerLootResolved(17U, 7U, 22UL, 1001U, 1),
                    new PlayerLootRejected(
                        17U,
                        8U,
                        PlayerLootRejectReason.AlreadyClaimed),
                    new PlayerInventorySnapshot(
                        22UL,
                        1,
                        10,
                        new[] { new PlayerInventoryEntry(1001U, 1) }));

                Assert.IsFalse(passed);
                StringAssert.Contains("Loot Race Pending", renderer.Text);
                StringAssert.Contains(
                    "Rejected: Missing AlreadyClaimed for same drop",
                    renderer.Text);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderCapturedEvidenceReturnsFalseWithoutCapturedSessionResults()
        {
            GameObject root = new GameObject("LootRaceEvidence");
            try
            {
                PlayerLootRaceEvidenceRenderer renderer =
                    root.AddComponent<PlayerLootRaceEvidenceRenderer>();

                Assert.IsFalse(renderer.RenderCapturedEvidence());
                Assert.AreEqual(string.Empty, renderer.Text);

                PlayerNetworkSessionController controller =
                    root.AddComponent<PlayerNetworkSessionController>();
                renderer.NetworkSessionController = controller;

                Assert.IsFalse(renderer.RenderCapturedEvidence());
                Assert.AreEqual(string.Empty, renderer.Text);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }
    }
}
