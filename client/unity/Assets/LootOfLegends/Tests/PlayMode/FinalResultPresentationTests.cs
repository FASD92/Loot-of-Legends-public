using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.FinalResult;
using LootOfLegends.Protocol;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using UnityEngine.UI;
using Object = UnityEngine.Object;

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
            var owner = new GameObject("FinalResultScreenViewTest");
            var view = owner.AddComponent<FinalResultScreenView>();
            try
            {
                var presenter = new FinalResultPresenter(
                    completion,
                    view,
                    new RecordingRoomReturnNavigation(new List<string>()),
                    LeaveOk);

                presenter.Render();
                yield return null;

                Assert.That(view.IsVisible, Is.True);
                Assert.That(view.Snapshot.HasWinner, Is.True);
                Assert.That(view.Snapshot.Rows.Select(row => row.Rank),
                    Is.EqualTo(new uint?[] { 1, 1, 3 }));
                Assert.That(view.Snapshot.Rows.Select(row => row.IsTop),
                    Is.EqualTo(new[] { true, true, false }));
            }
            finally
            {
                Object.DestroyImmediate(owner);
            }
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
                new RecordingRoomReturnNavigation(new List<string>()),
                LeaveOk);

            presenter.Render();
            yield return null;

            Assert.That(view.Last.Outcome,
                Is.EqualTo(FinalResultPresentationOutcome.CombatTimeout));
            Assert.That(view.Last.HasWinner, Is.False);
            Assert.That(view.Last.Rows.All(row => row.Rank == null && !row.IsTop), Is.True);
        }

        [UnityTest]
        public IEnumerator FinalResultRemainsUntilPlayerChoosesWhereToGo()
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
                new RecordingRoomReturnNavigation(sequence),
                LeaveOk);

            presenter.Render();
            presenter.Render();
            yield return null;

            Assert.That(sequence, Is.EqualTo(new[] { "result:9" }));
            presenter.ReturnToRoom();
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
                new RecordingRoomReturnNavigation(sequence),
                LeaveOk);

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
                "result:9", "hide", "result:10"
            }));
        }

        [Test]
        public async Task LobbyChoiceSubmitsOneLeaveIntentWhilePending()
        {
            int calls = 0;
            var leave = new TaskCompletionSource<RoomCommandResult>();
            var completion = new BattleResultReadModel();
            completion.BeginBattle(7, 9);
            completion.Apply(Result(9));
            completion.Apply(Room(firstReady: false, secondReady: false));
            var presenter = new FinalResultPresenter(
                completion,
                new RecordingFinalResultView(new List<string>()),
                new RecordingRoomReturnNavigation(new List<string>()),
                cancellationToken =>
                {
                    calls++;
                    return leave.Task;
                });

            Task first = presenter.ReturnToLobbyAsync(CancellationToken.None);
            Task second = presenter.ReturnToLobbyAsync(CancellationToken.None);

            Assert.That(calls, Is.EqualTo(1));
            leave.SetResult(RoomCommandResult.Ok);
            await Task.WhenAll(first, second);
        }

        [UnityTest]
        public IEnumerator ProductViewUsesLocalizedOutcomeCopiesAndVisibility()
        {
            SceneManager.LoadScene("ArenaScene");
            yield return null;
            FinalResultScreenView view =
                Object.FindFirstObjectByType<FinalResultScreenView>();
            GameObject overlay = BoundPanel(view);
            Text heading = NamedText(view.transform, "FinalResultHeading");
            Text outcome = NamedText(view.transform, "FinalResultOutcome");
            var audioSource = (AudioSource)typeof(FinalResultScreenView).GetField(
                "audioSource",
                BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(view);

            Assert.That(overlay.activeSelf, Is.False);
            Assert.That(audioSource, Is.Not.Null);
            foreach ((FinalResultPresentationOutcome value, string copy) in new[]
                     {
                         (FinalResultPresentationOutcome.MonsterDefeated, "몬스터 처치"),
                         (FinalResultPresentationOutcome.CombatTimeout, "전투 시간 초과"),
                         (FinalResultPresentationOutcome.CancelledNoActiveParticipants,
                             "전투 취소")
                     })
            {
                bool firstShow = !view.IsVisible;
                audioSource.Stop();
                view.Show(new FinalResultPresentationSnapshot(
                    7,
                    9,
                    value,
                    Array.Empty<FinalResultPresentationRow>()));
                Assert.That(overlay.activeSelf, Is.True);
                Assert.That(heading.text, Is.EqualTo("전투 결과"));
                Assert.That(outcome.text, Is.EqualTo(copy));
                Assert.That(audioSource.isPlaying, Is.EqualTo(firstShow));
            }

            view.Hide();
            audioSource.Stop();
            view.Show(new FinalResultPresentationSnapshot(
                7,
                10,
                FinalResultPresentationOutcome.MonsterDefeated,
                Array.Empty<FinalResultPresentationRow>()));
            Assert.That(audioSource.isPlaying, Is.True);
            view.Hide();
            yield return null;
            Assert.That(overlay.activeSelf, Is.False);
        }

        [UnityTest]
        public IEnumerator ProductViewPreservesServerRowsAndCleansPreviousBattle()
        {
            SceneManager.LoadScene("ArenaScene");
            yield return null;
            FinalResultScreenView view =
                Object.FindFirstObjectByType<FinalResultScreenView>();
            Transform list = NamedTransform(view.transform, "FinalResultList");
            FinalResultPresentationRow[] rows =
            {
                PresentationRow(1, "alpha", 700, 1, true),
                PresentationRow(2, "bravo", 700, 1, true),
                PresentationRow(3, "charlie", 590, 3, false),
                PresentationRow(4, "delta", 535, 4, false),
                PresentationRow(5, "echo", 480, 5, false),
                PresentationRow(6, "foxtrot", 425, 6, false),
                PresentationRow(7, "golf", 370, 7, false),
                PresentationRow(8, "hotel", 315, 8, false),
                PresentationRow(9, "india", 260, 9, false),
                PresentationRow(10, "juliet", 205, 10, false)
            };
            Assert.That(rows.Select(row => row.FinalAssetValue), Is.Ordered.Descending);
            Assert.That(rows.Select(row => row.Rank), Is.EqualTo(new uint?[]
            {
                1, 1, 3, 4, 5, 6, 7, 8, 9, 10
            }));
            Assert.That(rows[0].FinalAssetValue, Is.EqualTo(rows[1].FinalAssetValue));
            Assert.That(rows.Where(row => row.Rank == 1).Select(row => row.IsTop),
                Is.All.True);
            Assert.That(rows.Where(row => row.Rank != 1).Select(row => row.IsTop),
                Is.All.False);

            view.SetPlayerContext(1, 1);
            view.Show(new FinalResultPresentationSnapshot(
                7,
                9,
                FinalResultPresentationOutcome.MonsterDefeated,
                rows));
            yield return null;

            Transform[] rendered = ActiveRows(list);
            Assert.That(rendered, Has.Length.EqualTo(10));
            Assert.That(rendered.Select(row => NamedText(row, "NicknameLabel").text),
                Is.EqualTo(new[]
                {
                    "alpha (나·방장)", "bravo", "charlie", "delta", "echo",
                    "foxtrot", "golf", "hotel", "india", "juliet"
                }));
            Assert.That(NamedText(rendered[0], "RankLabel").text, Is.EqualTo("1위"));
            Assert.That(NamedText(rendered[1], "RankLabel").text, Is.EqualTo("1위"));
            Assert.That(NamedText(rendered[2], "RankLabel").text, Is.EqualTo("3위"));
            Assert.That(NamedText(rendered[0], "AssetValueLabel").text,
                Is.EqualTo("700"));
            Assert.That(NamedText(rendered[0], "TopLabel").gameObject.activeSelf, Is.True);
            Assert.That(NamedText(rendered[1], "TopLabel").gameObject.activeSelf, Is.True);
            Assert.That(NamedText(rendered[2], "TopLabel").gameObject.activeSelf, Is.False);
            Assert.That(rendered[0].GetComponent<Image>().enabled, Is.True);
            Assert.That(rendered[1].GetComponent<Image>().enabled, Is.True);
            Assert.That(rendered[2].GetComponent<Image>().enabled, Is.False);
            Assert.That(rendered[0].GetComponent<Image>().sprite, Is.Null);
            Assert.That(rendered[0].GetComponent<Image>().color,
                Is.EqualTo(rendered[1].GetComponent<Image>().color));
            Assert.That(NamedImage(rendered[0], "RowSeparator").sprite, Is.Null);

            FinalResultPresentationRow[] timeoutRows = Enumerable.Range(0, 10)
                .Select(index => PresentationRow(
                    (ulong)(11 + index),
                    "timeout-" + index,
                    0,
                    null,
                    false))
                .ToArray();
            Assert.That(timeoutRows.Select(row => row.FinalAssetValue),
                Is.All.EqualTo(0UL));
            Assert.That(timeoutRows.Select(row => row.Rank), Is.All.Null);
            Assert.That(timeoutRows.Select(row => row.IsTop), Is.All.False);

            view.Show(new FinalResultPresentationSnapshot(
                7,
                10,
                FinalResultPresentationOutcome.CombatTimeout,
                timeoutRows));
            yield return null;

            rendered = ActiveRows(list);
            Assert.That(rendered, Has.Length.EqualTo(10));
            Assert.That(NamedText(rendered[0], "NicknameLabel").text,
                Is.EqualTo("timeout-0"));
            Assert.That(rendered.Select(row => NamedText(row, "RankLabel").text),
                Is.All.EqualTo("—"));
            Assert.That(rendered.Select(row => NamedText(row, "AssetValueLabel").text),
                Is.All.EqualTo("0"));
            Assert.That(rendered.Select(row =>
                    NamedText(row, "TopLabel").gameObject.activeSelf),
                Is.All.False);
            Assert.That(rendered.Select(row => row.GetComponent<Image>().enabled),
                Is.All.False);

            view.Hide();
            yield return null;
            Assert.That(ActiveRows(list), Is.Empty);
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

        private static Task<RoomCommandResult> LeaveOk(
            CancellationToken cancellationToken)
        {
            return Task.FromResult(RoomCommandResult.Ok);
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

        private static FinalResultPresentationRow PresentationRow(
            ulong sessionId,
            string nickname,
            ulong value,
            uint? rank,
            bool isTop)
        {
            return new FinalResultPresentationRow(
                sessionId,
                nickname,
                FinalResultPresentationExitStatus.TerminalPresent,
                value,
                rank,
                isTop);
        }

        private static Transform NamedTransform(Transform root, string name)
        {
            Transform found = root.GetComponentsInChildren<Transform>(true)
                .SingleOrDefault(child => child.name == name);
            Assert.That(found, Is.Not.Null, name + " is required");
            return found;
        }

        private static Text NamedText(Transform root, string name)
        {
            Text found = root.GetComponentsInChildren<Text>(true)
                .SingleOrDefault(label => label.name == name);
            Assert.That(found, Is.Not.Null, name + " is required");
            return found;
        }

        private static Image NamedImage(Transform root, string name)
        {
            Image found = root.GetComponentsInChildren<Image>(true)
                .SingleOrDefault(image => image.name == name);
            Assert.That(found, Is.Not.Null, name + " is required");
            return found;
        }

        private static Transform[] ActiveRows(Transform list)
        {
            return list.Cast<Transform>()
                .Where(row => row.gameObject.activeSelf)
                .ToArray();
        }

        private static GameObject BoundPanel(FinalResultScreenView view)
        {
            FieldInfo field = typeof(FinalResultScreenView).GetField(
                "panel",
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null);
            var panel = (GameObject)field.GetValue(view);
            Assert.That(panel, Is.Not.Null);
            return panel;
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

            public void SetActions(bool enabled, string statusCopy)
            {
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
