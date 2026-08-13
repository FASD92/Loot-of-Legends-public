using System;
using System.Collections;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Presentation.Common;
using LootOfLegends.Presentation.FinalResult;
using LootOfLegends.Protocol;
using NUnit.Framework;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class BattleRecoveryPresentationTests
    {
        [UnityTest]
        public IEnumerator ResultFailureShowsNoResultAndWaitsForServerLobbyReturn()
        {
            var load = ActiveLoad(7, 9);
            var result = new BattleResultReadModel();
            result.BeginBattle(7, 9);
            var router = new BattleCompletionRouter(
                new BattleResponseCorrelator(), load, result);
            var recoveryView = new RecordingRecoveryView();
            var lobby = new RecordingRecoveryLobbyNavigation();
            var resultView = new RecordingFinalResultView();
            var presenter = new BattleRecoveryPresenter(result, recoveryView, lobby);
            var finalPresenter = new FinalResultPresenter(
                result, resultView, new RecordingRoomReturnNavigation());

            var notice = new BattleRecoveryNotice(
                7, 9, BattleRecoveryReason.ResultGenerationFailed);
            router.OnMessage(notice);
            router.OnMessage(notice);
            presenter.Render();
            finalPresenter.Render();

            Assert.That(load.IsGameplayActive, Is.False);
            Assert.That(result.RecoveryState,
                Is.EqualTo(BattleRecoveryState.ResultGenerationFailed));
            Assert.That(result.HasFinalResult, Is.False);
            Assert.That(result.Snapshot(), Is.Null);
            Assert.That(recoveryView.ShowCalls, Is.EqualTo(1));
            Assert.That(recoveryView.Copy,
                Is.EqualTo("결과를 안전하게 확정하지 못했습니다."));
            Assert.That(resultView.ShowCalls, Is.EqualTo(0));
            Assert.That(lobby.Calls, Is.EqualTo(0));
            Assert.That(result.Apply(Result(7, 9)), Is.False,
                "late FinalResult must not repair a failed result");

            router.OnMessage(new LobbyEntrySnapshot(
                1, 1, "neo", Array.Empty<RoomSummary>()));
            presenter.Render();
            presenter.Render();
            Assert.That(lobby.Calls, Is.EqualTo(1));
            Assert.That(result.IsLobbyReturnConfirmed, Is.True);
            yield return null;
        }

        [UnityTest]
        public IEnumerator StoragePendingDisablesInputUntilFinalResultThenRoomOpen()
        {
            var load = ActiveLoad(7, 10);
            var result = new BattleResultReadModel();
            result.BeginBattle(7, 10);
            var router = new BattleCompletionRouter(
                new BattleResponseCorrelator(), load, result);
            var recoveryView = new RecordingRecoveryView();
            var lobby = new RecordingRecoveryLobbyNavigation();
            var recoveryPresenter = new BattleRecoveryPresenter(
                result, recoveryView, lobby);
            var resultView = new RecordingFinalResultView();
            var roomReturn = new RecordingRoomReturnNavigation();
            var resultPresenter = new FinalResultPresenter(
                result, resultView, roomReturn);

            var pending = new BattleRecoveryNotice(
                7, 10, BattleRecoveryReason.SettlementRecoveryPending);
            router.OnMessage(pending);
            router.OnMessage(pending);
            recoveryPresenter.Render();

            Assert.That(load.IsGameplayActive, Is.False);
            Assert.That(result.RecoveryState,
                Is.EqualTo(BattleRecoveryState.SettlementRecoveryPending));
            Assert.That(recoveryView.ShowCalls, Is.EqualTo(1));
            Assert.That(recoveryView.Copy,
                Is.EqualTo("결과를 안전하게 저장 중입니다. 잠시 기다려 주세요."));
            Assert.That(lobby.Calls, Is.EqualTo(0));
            AssertInputDisabled(load);

            router.OnMessage(Result(7, 10));
            recoveryPresenter.Render();
            resultPresenter.Render();
            Assert.That(result.RecoveryState, Is.EqualTo(BattleRecoveryState.None));
            Assert.That(recoveryView.HideCalls, Is.EqualTo(1));
            Assert.That(resultView.ShowCalls, Is.EqualTo(1));
            Assert.That(roomReturn.Calls, Is.EqualTo(0));

            router.OnMessage(AllUnreadyRoom(7));
            resultPresenter.Render();
            Assert.That(roomReturn.Calls, Is.EqualTo(1));
            Assert.That(lobby.Calls, Is.EqualTo(0));
            yield return null;
        }

        [Test]
        public void StaleAndFutureNoticesDoNotMutateCurrentBattle()
        {
            var load = ActiveLoad(7, 10);
            var result = new BattleResultReadModel();
            result.BeginBattle(7, 10);
            var router = new BattleCompletionRouter(
                new BattleResponseCorrelator(), load, result);

            router.OnMessage(new BattleRecoveryNotice(
                7, 9, BattleRecoveryReason.ResultGenerationFailed));
            router.OnMessage(new BattleRecoveryNotice(
                7, 11, BattleRecoveryReason.SettlementRecoveryPending));
            router.OnMessage(new BattleRecoveryNotice(
                8, 10, BattleRecoveryReason.ResultGenerationFailed));

            Assert.That(result.RecoveryState, Is.EqualTo(BattleRecoveryState.None));
            Assert.That(load.IsGameplayActive, Is.True);
        }

        private static BattleLoadReadModel ActiveLoad(ulong roomId, ulong battleId)
        {
            var load = new BattleLoadReadModel();
            load.Apply(new ArenaLoadEntry(roomId, battleId));
            load.Apply(new ArenaGameplayStart(
                roomId,
                battleId,
                new[]
                {
                    new BattleParticipant(1, 1, "neo"),
                    new BattleParticipant(2, 1, "trinity")
                }));
            return load;
        }

        private static void AssertInputDisabled(BattleLoadReadModel load)
        {
            var input = new ArenaInputFacade(
                load,
                new BattleCombatReadModel(load.BattleInstanceId),
                new BattleLootReadModel(load.BattleInstanceId),
                (_, _, _) => Task.CompletedTask,
                (_, _) => Task.CompletedTask,
                (_, _) => Task.CompletedTask);
            Assert.Throws<ArenaInputUnavailableException>(() =>
                input.MoveAsync(1, 1, CancellationToken.None));
        }

        private static BattleFinalResult Result(ulong roomId, ulong battleId)
        {
            return new BattleFinalResult(
                roomId,
                battleId,
                FinalResultOutcome.MonsterDefeated,
                new[]
                {
                    new FinalResultEntry(
                        1, "neo", FinalResultExitStatus.TerminalPresent,
                        300, 1, true),
                    new FinalResultEntry(
                        2, "trinity", FinalResultExitStatus.TerminalPresent,
                        100, 2, false)
                });
        }

        private static RoomDetailProjection AllUnreadyRoom(ulong roomId)
        {
            return new RoomDetailProjection(
                roomId,
                "room",
                2,
                1,
                1,
                new[]
                {
                    new RoomMember(1, 1, "neo", false),
                    new RoomMember(2, 1, "trinity", false)
                });
        }

        private sealed class RecordingRecoveryView : IBattleRecoveryView
        {
            public int ShowCalls { get; private set; }
            public int HideCalls { get; private set; }
            public string Copy { get; private set; }

            public void ShowBlockingMessage(string copy)
            {
                ShowCalls++;
                Copy = copy;
            }

            public void HideBlockingMessage()
            {
                HideCalls++;
            }
        }

        private sealed class RecordingRecoveryLobbyNavigation :
            IBattleRecoveryLobbyNavigation
        {
            public int Calls { get; private set; }

            public void ReturnToLobby()
            {
                Calls++;
            }
        }

        private sealed class RecordingFinalResultView : IFinalResultView
        {
            public int ShowCalls { get; private set; }

            public void Show(FinalResultPresentationSnapshot snapshot)
            {
                ShowCalls++;
            }

            public void Hide()
            {
            }
        }

        private sealed class RecordingRoomReturnNavigation : IRoomReturnNavigation
        {
            public int Calls { get; private set; }

            public void ReturnToRoom(ulong roomId)
            {
                Calls++;
            }
        }
    }
}
