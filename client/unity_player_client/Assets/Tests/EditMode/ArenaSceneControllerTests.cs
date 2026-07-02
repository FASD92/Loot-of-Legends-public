using System.Threading.Tasks;
using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class ArenaSceneControllerTests
    {
        [Test]
        public void EnterPreStartWaitingBlocksInputAndShowsWaitingCopy()
        {
            GameObject host = new GameObject("ArenaSceneControllerHost");
            try
            {
                ArenaSceneController controller = host.AddComponent<ArenaSceneController>();

                controller.EnterPreStartWaiting(42U, 9001UL);

                Assert.IsTrue(controller.WaitingOverlayVisible);
                Assert.AreEqual(ArenaSceneController.WaitingOverlayText, controller.WaitingText);
                Assert.IsFalse(controller.GameplayInputEnabled);
                Assert.IsFalse(controller.SessionBoundaryFailed);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void ApplyArenaGameplayStartForCurrentBattleEnablesInput()
        {
            GameObject host = new GameObject("ArenaSceneControllerHost");
            try
            {
                ArenaSceneController controller = host.AddComponent<ArenaSceneController>();
                controller.EnterPreStartWaiting(42U, 9001UL);

                controller.ApplyArenaGameplayStart(42U, 9001UL);

                Assert.IsFalse(controller.WaitingOverlayVisible);
                Assert.AreEqual(string.Empty, controller.WaitingText);
                Assert.IsTrue(controller.GameplayInputEnabled);
                Assert.IsFalse(controller.SessionBoundaryFailed);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void ApplyArenaGameplayStartForMismatchedBattleFailsSessionBoundary()
        {
            GameObject host = new GameObject("ArenaSceneControllerHost");
            try
            {
                ArenaSceneController controller = host.AddComponent<ArenaSceneController>();
                controller.EnterPreStartWaiting(42U, 9001UL);

                controller.ApplyArenaGameplayStart(42U, 9002UL);

                Assert.IsTrue(controller.WaitingOverlayVisible);
                Assert.IsFalse(controller.GameplayInputEnabled);
                Assert.IsTrue(controller.SessionBoundaryFailed);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void ApplyBattleFinalRankingShowsFinalRankingOverlay()
        {
            GameObject host = new GameObject("ArenaSceneControllerHost");
            try
            {
                ArenaSceneController controller = host.AddComponent<ArenaSceneController>();
                controller.EnterPreStartWaiting(42U, 9001UL);
                controller.ApplyArenaGameplayStart(42U, 9001UL);

                controller.ApplyBattleFinalRanking(new PlayerBattleFinalRanking(
                    42U,
                    9001UL,
                    new[]
                    {
                        new PlayerBattleFinalRankingRow(1, 22UL, "Player22", 100L),
                        new PlayerBattleFinalRankingRow(2, 11UL, "Player11", 0L)
                    }));

                Assert.IsTrue(controller.FinalRankingOverlayVisible);
                Assert.AreEqual(
                    ArenaSceneController.FinalRankingTitleText,
                    controller.FinalRankingTitle);
                Assert.IsFalse(controller.GameplayInputEnabled);
                Assert.IsFalse(controller.LobbyReturnButtonEnabled);
                Assert.AreEqual(2, controller.VisibleFinalRankingRows.Length);
                Assert.AreEqual(1, controller.VisibleFinalRankingRows[0].Rank);
                Assert.AreEqual("Player22", controller.VisibleFinalRankingRows[0].Nickname);
                Assert.AreEqual(100L, controller.VisibleFinalRankingRows[0].TotalAssetValue);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void ApplyLobbyReturnVisibilityEnablesLobbyButtonAfterFinalRanking()
        {
            GameObject host = new GameObject("ArenaSceneControllerHost");
            try
            {
                ArenaSceneController controller = host.AddComponent<ArenaSceneController>();
                controller.EnterPreStartWaiting(42U, 9001UL);
                controller.ApplyArenaGameplayStart(42U, 9001UL);
                controller.ApplyBattleFinalRanking(new PlayerBattleFinalRanking(
                    42U,
                    9001UL,
                    new[]
                    {
                        new PlayerBattleFinalRankingRow(1, 22UL, "Player22", 100L)
                    }));

                controller.ApplyLobbyReturnVisibility(42U, PlayerLobbyReturnReason.None);

                Assert.IsTrue(controller.FinalRankingOverlayVisible);
                Assert.IsTrue(controller.LobbyReturnButtonEnabled);
                Assert.IsFalse(controller.SessionBoundaryFailed);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public void CalculateFinalRankingOverlayRectCentersWithinScaledGuiCoordinates()
        {
            Rect rect = ArenaSceneController.CalculateFinalRankingOverlayRect(1366, 768);
            float scale = PlayerManualGuiScaler.CalculateScale(768);

            float visualCenterX = (rect.x + rect.width * 0.5f) * scale;
            float visualCenterY = (rect.y + rect.height * 0.5f) * scale;

            Assert.AreEqual(1366.0f * 0.5f, visualCenterX, 0.5f);
            Assert.AreEqual(768.0f * 0.5f, visualCenterY, 0.5f);
        }

        [Test]
        public async Task CompleteLocalLoadAsyncReturnsFalseWithoutNetworkController()
        {
            GameObject host = new GameObject("ArenaSceneControllerHost");
            try
            {
                ArenaSceneController controller = host.AddComponent<ArenaSceneController>();
                controller.EnterPreStartWaiting(42U, 9001UL);

                bool completed = await controller.CompleteLocalLoadAsync();

                Assert.IsFalse(completed);
                Assert.IsTrue(controller.WaitingOverlayVisible);
                Assert.IsFalse(controller.GameplayInputEnabled);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }
    }
}
