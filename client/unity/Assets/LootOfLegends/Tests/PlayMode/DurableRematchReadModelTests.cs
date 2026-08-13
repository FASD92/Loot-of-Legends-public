using System.Collections;
using System.Linq;
using LootOfLegends.Battle;
using LootOfLegends.Collection;
using LootOfLegends.Protocol;
using NUnit.Framework;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class DurableRematchReadModelTests
    {
        [UnityTest]
        public IEnumerator ResultThenAllUnreadyRoomAllowsSecondBattle()
        {
            var completion = new BattleResultReadModel();
            var router = new BattleCompletionRouter(
                new BattleResponseCorrelator(),
                new BattleLoadReadModel(),
                completion);

            router.OnMessage(new ArenaLoadEntry(7, 9));
            router.OnMessage(Result(9));
            Assert.That(completion.HasFinalResult, Is.True);
            Assert.That(completion.IsReadyForRematch, Is.False);
            yield return null;

            router.OnMessage(Room(firstReady: true, secondReady: false));
            Assert.That(completion.IsReadyForRematch, Is.False);

            router.OnMessage(Room(firstReady: false, secondReady: false));
            Assert.That(completion.IsReadyForRematch, Is.True);
            Assert.That(completion.CurrentBattleInstanceId, Is.EqualTo(9));
            yield return null;

            router.OnMessage(new ArenaLoadEntry(7, 10));
            Assert.That(completion.CurrentBattleInstanceId, Is.EqualTo(10));
            Assert.That(completion.HasFinalResult, Is.False);
            Assert.That(completion.IsReadyForRematch, Is.False);
        }

        [UnityTest]
        public IEnumerator PendingCollectionNeverPredictsAppliedQuantity()
        {
            var model = new CollectionReadModel();
            model.Apply(new CollectionSnapshot(
                new[] { new CollectionItem(1, 4, 100) }, 0, 1));
            yield return null;

            Assert.That(model.PendingSettlementCount, Is.EqualTo(1));
            Assert.That(model.Items.Single().Quantity, Is.EqualTo(4));

            model.Apply(new CollectionSnapshot(
                new[] { new CollectionItem(1, 4, 100) }, 0, 2));
            yield return null;

            Assert.That(model.PendingSettlementCount, Is.EqualTo(2));
            Assert.That(model.Items.Single().Quantity, Is.EqualTo(4));
        }

        private static BattleFinalResult Result(ulong battleInstanceId)
        {
            return new BattleFinalResult(
                7,
                battleInstanceId,
                FinalResultOutcome.MonsterDefeated,
                new[]
                {
                    new FinalResultEntry(
                        1, "neo", FinalResultExitStatus.TerminalPresent, 300, 1, true),
                    new FinalResultEntry(
                        2, "trinity", FinalResultExitStatus.TerminalPresent, 100, 2, false)
                });
        }

        private static RoomDetailProjection Room(bool firstReady, bool secondReady)
        {
            return new RoomDetailProjection(
                7,
                "room",
                2,
                1,
                2,
                new[]
                {
                    new RoomMember(1, 2, "neo", firstReady),
                    new RoomMember(2, 3, "trinity", secondReady)
                });
        }
    }
}
