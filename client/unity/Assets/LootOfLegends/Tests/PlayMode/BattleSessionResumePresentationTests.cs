using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Protocol;
using NUnit.Framework;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class BattleSessionResumePresentationTests
    {
        [Test]
        public void ReconnectLockPreventsMovementUntilSnapshotApplyCompletes()
        {
            var load = new BattleLoadReadModel();
            load.Apply(new ArenaLoadEntry(7, 9));
            load.Apply(new ArenaGameplayStart(
                7,
                9,
                new[] { new BattleParticipant(1, 2, "neo") }));
            var input = new ArenaInputFacade(
                load,
                new BattleCombatReadModel(9),
                new BattleLootReadModel(9),
                (_, _, _) => Task.CompletedTask,
                (_, _) => Task.CompletedTask,
                (_, _) => Task.CompletedTask);

            load.SetReconnectLocked(true);
            Assert.Throws<ArenaInputUnavailableException>(() =>
                input.MoveAsync(1, 0, CancellationToken.None));

            load.SetReconnectLocked(false);
            Assert.DoesNotThrowAsync(async () =>
                await input.MoveAsync(1, 0, CancellationToken.None));
        }

        [Test]
        public void ResultSnapshotKeepsTheBattleResultWithoutRestartingArena()
        {
            var load = new BattleLoadReadModel();
            var result = new BattleResultReadModel();
            BattleResumeSnapshot snapshot = ResultSnapshot();

            Assert.That(load.ApplyResumeSnapshot(snapshot), Is.True);
            Assert.That(result.ApplyResumeSnapshot(snapshot), Is.True);
            Assert.That(load.IsGameplayActive, Is.False);
            Assert.That(load.BattleInstanceId, Is.EqualTo(9));
            Assert.That(result.HasFinalResult, Is.True);
            Assert.That(result.Snapshot().Outcome,
                Is.EqualTo(FinalResultPresentationOutcome.MonsterDefeated));
            Assert.That(result.CurrentRoomId, Is.EqualTo(7));
            Assert.That(result.CurrentBattleInstanceId, Is.EqualTo(9));
        }

        private static BattleResumeSnapshot ResultSnapshot()
        {
            return new BattleResumeSnapshot(
                5,
                6,
                7,
                9,
                1,
                2,
                BattleResumePhase.Result,
                0,
                99,
                new[]
                {
                    new BattleResumePlayerState(1, 100, -20, true, 80, 100, true),
                    new BattleResumePlayerState(2, 0, 0, true, 70, 100, true)
                },
                null,
                Array.Empty<BattleResumeDropState>(),
                123,
                new BattleResumeResultState(
                    FinalResultOutcome.MonsterDefeated,
                    new[]
                    {
                        new FinalResultEntry(
                            1,
                            "neo",
                            FinalResultExitStatus.TerminalPresent,
                            100,
                            1,
                            true),
                        new FinalResultEntry(
                            2,
                            "trinity",
                            FinalResultExitStatus.TerminalExited,
                            80,
                            2,
                            false)
                    }));
        }
    }
}
