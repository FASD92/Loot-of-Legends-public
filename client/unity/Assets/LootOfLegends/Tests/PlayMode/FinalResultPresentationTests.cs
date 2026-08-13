using System.Collections;
using System.Collections.Generic;
using System.Linq;
using LootOfLegends.Battle;
using LootOfLegends.Presentation.FinalResult;
using LootOfLegends.Protocol;
using NUnit.Framework;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class FinalResultPresentationTests
    {
        [UnityTest]
        public IEnumerator MonsterDefeatedPreservesServerTieRanksAndTopRows()
        {
            var completion = new BattleResultReadModel();
            completion.BeginBattle(7, 9);
            completion.Apply(new BattleFinalResult(
                7,
                9,
                FinalResultOutcome.MonsterDefeated,
                new[]
                {
                    Entry(1, "neo", 500, 1, true),
                    Entry(2, "trinity", 500, 1, true),
                    Entry(3, "morpheus", 100, 3, false)
                }));
            var view = new RecordingFinalResultView(new List<string>());
            var presenter = new FinalResultPresenter(
                completion,
                view,
                new RecordingRoomReturnNavigation(new List<string>()));

            presenter.Render();
            yield return null;

            Assert.That(view.Last.HasWinner, Is.True);
            Assert.That(view.Last.Rows.Select(row => row.Rank),
                Is.EqualTo(new uint?[] { 1, 1, 3 }));
            Assert.That(view.Last.Rows.Select(row => row.IsTop),
                Is.EqualTo(new[] { true, true, false }));
        }

        [UnityTest]
        public IEnumerator CombatTimeoutShowsNoWinnerWithoutLocalInference()
        {
            var completion = new BattleResultReadModel();
            completion.BeginBattle(7, 9);
            completion.Apply(new BattleFinalResult(
                7,
                9,
                FinalResultOutcome.CombatTimeout,
                new[]
                {
                    Entry(1, "neo", 300, null, false),
                    Entry(2, "trinity", 100, null, false)
                }));
            var view = new RecordingFinalResultView(new List<string>());
            var presenter = new FinalResultPresenter(
                completion,
                view,
                new RecordingRoomReturnNavigation(new List<string>()));

            presenter.Render();
            yield return null;

            Assert.That(view.Last.Outcome,
                Is.EqualTo(FinalResultPresentationOutcome.CombatTimeout));
            Assert.That(view.Last.HasWinner, Is.False);
            Assert.That(view.Last.Rows.All(row => row.Rank == null && !row.IsTop), Is.True);
        }

        [UnityTest]
        public IEnumerator FinalResultIsPresentedBeforeAllUnreadyRoomReturn()
        {
            var sequence = new List<string>();
            var completion = new BattleResultReadModel();
            completion.BeginBattle(7, 9);
            completion.Apply(Result(9));
            Assert.That(completion.Apply(Room(firstReady: true, secondReady: false)), Is.False);
            Assert.That(completion.Apply(Room(firstReady: false, secondReady: false)), Is.True);
            var presenter = new FinalResultPresenter(
                completion,
                new RecordingFinalResultView(sequence),
                new RecordingRoomReturnNavigation(sequence));

            presenter.Render();
            presenter.Render();
            yield return null;

            Assert.That(sequence, Is.EqualTo(new[] { "result:9", "room:7" }));
        }

        [UnityTest]
        public IEnumerator SecondBattleInstanceGetsIndependentResultAndRoomReturn()
        {
            var sequence = new List<string>();
            var completion = new BattleResultReadModel();
            var presenter = new FinalResultPresenter(
                completion,
                new RecordingFinalResultView(sequence),
                new RecordingRoomReturnNavigation(sequence));

            completion.BeginBattle(7, 9);
            completion.Apply(Result(9));
            completion.Apply(Room(firstReady: false, secondReady: false));
            presenter.Render();

            completion.BeginBattle(7, 10);
            presenter.Render();
            completion.Apply(Result(10));
            completion.Apply(Room(firstReady: false, secondReady: false));
            presenter.Render();
            yield return null;

            Assert.That(sequence, Is.EqualTo(new[]
            {
                "result:9", "room:7", "hide", "result:10", "room:7"
            }));
        }

        private static BattleFinalResult Result(ulong battleInstanceId)
        {
            return new BattleFinalResult(
                7,
                battleInstanceId,
                FinalResultOutcome.MonsterDefeated,
                new[]
                {
                    Entry(1, "neo", 300, 1, true),
                    Entry(2, "trinity", 100, 2, false)
                });
        }

        private static FinalResultEntry Entry(
            ulong sessionId,
            string nickname,
            ulong value,
            uint? rank,
            bool isTop)
        {
            return new FinalResultEntry(
                sessionId,
                nickname,
                FinalResultExitStatus.TerminalPresent,
                value,
                rank,
                isTop);
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

        private sealed class RecordingFinalResultView : IFinalResultView
        {
            private readonly IList<string> sequence;

            public RecordingFinalResultView(IList<string> sequence)
            {
                this.sequence = sequence;
            }

            public FinalResultPresentationSnapshot Last { get; private set; }

            public void Show(FinalResultPresentationSnapshot snapshot)
            {
                Last = snapshot;
                sequence.Add("result:" + snapshot.BattleInstanceId);
            }

            public void Hide()
            {
                sequence.Add("hide");
            }
        }

        private sealed class RecordingRoomReturnNavigation : IRoomReturnNavigation
        {
            private readonly IList<string> sequence;

            public RecordingRoomReturnNavigation(IList<string> sequence)
            {
                this.sequence = sequence;
            }

            public void ReturnToRoom(ulong roomId)
            {
                sequence.Add("room:" + roomId);
            }
        }
    }
}
