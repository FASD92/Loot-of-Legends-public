using System.Collections;
using LootOfLegends.Battle;
using LootOfLegends.Protocol;
using LootOfLegends.Presentation;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class BattleLoadWaitingOverlayTests
    {
        [UnityTest]
        public IEnumerator EntryAndCancellationToggleWaitingOverlay()
        {
            var gameObject = new GameObject("Battle load waiting overlay");
            CanvasGroup canvas = gameObject.AddComponent<CanvasGroup>();
            BattleLoadWaitingOverlay overlay = gameObject.AddComponent<BattleLoadWaitingOverlay>();
            var readModel = new BattleLoadReadModel();

            readModel.Apply(new ArenaLoadEntry(7, 1));
            overlay.Render(readModel);
            yield return null;

            Assert.That(canvas.alpha, Is.EqualTo(1));
            Assert.That(canvas.blocksRaycasts, Is.True);

            readModel.Apply(new ArenaLoadCancelled(
                7,
                1,
                ArenaLoadCancelReason.NotEnoughReady));
            overlay.Render(readModel);
            yield return null;

            Assert.That(canvas.alpha, Is.EqualTo(0));
            Assert.That(canvas.blocksRaycasts, Is.False);
            Object.Destroy(gameObject);
        }
    }
}
