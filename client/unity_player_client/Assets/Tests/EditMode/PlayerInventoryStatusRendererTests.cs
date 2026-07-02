using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerInventoryStatusRendererTests
    {
        [Test]
        public void RenderInventorySnapshotShowsWeightAndEntries()
        {
            GameObject root = new GameObject("InventoryStatus");
            try
            {
                PlayerInventoryStatusRenderer renderer =
                    root.AddComponent<PlayerInventoryStatusRenderer>();

                renderer.RenderInventorySnapshot(new PlayerInventorySnapshot(
                    22UL,
                    3,
                    10,
                    new[]
                    {
                        new PlayerInventoryEntry(1001U, 1),
                        new PlayerInventoryEntry(1002U, 2)
                    }));

                Assert.AreEqual(
                    "Inventory 3/10\nItem 1001 x1\nItem 1002 x2",
                    renderer.Text);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderInventorySnapshotShowsEmptyAndClearRemovesText()
        {
            GameObject root = new GameObject("InventoryStatus");
            try
            {
                PlayerInventoryStatusRenderer renderer =
                    root.AddComponent<PlayerInventoryStatusRenderer>();

                renderer.RenderInventorySnapshot(new PlayerInventorySnapshot(
                    22UL,
                    0,
                    10,
                    new PlayerInventoryEntry[0]));

                Assert.AreEqual("Inventory 0/10\nEmpty", renderer.Text);

                renderer.Clear();

                Assert.AreEqual(string.Empty, renderer.Text);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderInventorySnapshotClearsDefaultSnapshot()
        {
            GameObject root = new GameObject("InventoryStatus");
            try
            {
                PlayerInventoryStatusRenderer renderer =
                    root.AddComponent<PlayerInventoryStatusRenderer>();
                renderer.RenderInventorySnapshot(new PlayerInventorySnapshot(
                    22UL,
                    1,
                    10,
                    new[] { new PlayerInventoryEntry(1001U, 1) }));

                renderer.RenderInventorySnapshot(default);

                Assert.AreEqual(string.Empty, renderer.Text);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderCapturedInventorySnapshotReturnsFalseWithoutCapturedResult()
        {
            GameObject root = new GameObject("InventoryStatus");
            try
            {
                PlayerInventoryStatusRenderer renderer =
                    root.AddComponent<PlayerInventoryStatusRenderer>();

                Assert.IsFalse(renderer.RenderCapturedInventorySnapshot());
                Assert.AreEqual(string.Empty, renderer.Text);

                PlayerNetworkSessionController controller =
                    root.AddComponent<PlayerNetworkSessionController>();
                renderer.NetworkSessionController = controller;

                Assert.IsFalse(renderer.RenderCapturedInventorySnapshot());
                Assert.AreEqual(string.Empty, renderer.Text);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }
    }
}
