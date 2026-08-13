using System.Collections;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Protocol;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class ArenaNormalFlowPresentationTests
    {
        [UnityTest]
        public IEnumerator WaitingOverlayBlocksAllInputsUntilServerGameplayStart()
        {
            var load = new BattleLoadReadModel();
            var combat = new BattleCombatReadModel(9);
            var loot = new BattleLootReadModel(9);
            int moves = 0;
            int attacks = 0;
            int claims = 0;
            var input = new ArenaInputFacade(
                load,
                combat,
                loot,
                (x, y, cancellation) => { moves++; return Task.CompletedTask; },
                (target, cancellation) => { attacks++; return Task.CompletedTask; },
                (drop, cancellation) => { claims++; return Task.CompletedTask; });
            var view = new RecordingArenaView();
            var presenter = new ArenaPresenter(
                new ArenaPlayerFlowReadModel(
                    load,
                    new BattleMovementReadModel(9),
                    combat,
                    loot),
                input,
                view);

            load.Apply(new ArenaLoadEntry(7, 9));
            presenter.Render();
            Assert.That(view.Last.WaitingForGameplayStart, Is.True);
            Assert.That(view.Last.ControlsEnabled, Is.False);
            Assert.ThrowsAsync<ArenaInputUnavailableException>(async () =>
                await presenter.MoveAsync(1, 0, CancellationToken.None));
            Assert.ThrowsAsync<ArenaInputUnavailableException>(async () =>
                await presenter.AttackAsync(1, CancellationToken.None));
            Assert.ThrowsAsync<ArenaInputUnavailableException>(async () =>
                await presenter.ClaimAsync(1, CancellationToken.None));
            Assert.That(moves + attacks + claims, Is.Zero);

            load.Apply(GameplayStart());
            combat.Apply(new RudpMonsterSpawned(
                new RudpEventId(1, 1),
                9,
                RudpEventStreamKind.CombatLifecycle,
                1,
                1,
                0,
                0,
                1600,
                1));
            loot.Apply(new RudpDropStateSnapshot(
                9,
                1,
                RudpLootResolutionState.Open,
                new[]
                {
                    new RudpLootDropProjection(
                        3, 2, 1, 1000, 2000, RudpLootDropState.Available, 0)
                }));
            presenter.Render();
            Assert.That(view.Last.ControlsEnabled, Is.True);

            Task move = presenter.MoveAsync(1, -1, CancellationToken.None);
            Task attack = presenter.AttackAsync(1, CancellationToken.None);
            Task claim = presenter.ClaimAsync(3, CancellationToken.None);
            yield return new WaitUntil(() => move.IsCompleted && attack.IsCompleted && claim.IsCompleted);

            Assert.That(moves, Is.EqualTo(1));
            Assert.That(attacks, Is.EqualTo(1));
            Assert.That(claims, Is.EqualTo(1));
            Assert.That(view.Last.Players, Is.Empty,
                "input submission must not create a predicted position");
        }

        [UnityTest]
        public IEnumerator MovementMonsterDropAndTerminalCopyComeOnlyFromServerModels()
        {
            var load = new BattleLoadReadModel();
            load.Apply(new ArenaLoadEntry(7, 9));
            load.Apply(GameplayStart());
            var movement = new BattleMovementReadModel(9);
            movement.Apply(new RudpStateSnapshot(
                9,
                2,
                60,
                new[] { new RudpSnapshotPlayer(1, 1250, -1750) }));
            var combat = new BattleCombatReadModel(9);
            combat.Apply(new RudpMonsterSpawned(
                new RudpEventId(1, 1),
                9,
                RudpEventStreamKind.CombatLifecycle,
                1,
                1,
                0,
                0,
                1600,
                1));
            combat.Apply(new RudpAttackTerminalResult(
                RudpCommandId.Create(),
                9,
                RudpAttackResultCode.OutOfRange,
                1,
                1600,
                1,
                RudpCombatOutcome.None));
            var loot = new BattleLootReadModel(9);
            loot.Apply(new RudpDropStateSnapshot(
                9,
                1,
                RudpLootResolutionState.Open,
                new[]
                {
                    new RudpLootDropProjection(
                        3, 2, 1, 3000, 4000, RudpLootDropState.Available, 0)
                }));
            loot.Apply(new RudpClaimLootTerminalResult(
                RudpCommandId.Create(),
                9,
                3,
                RudpClaimLootResultCode.AlreadyClaimed));
            var view = new RecordingArenaView();
            var presenter = new ArenaPresenter(
                new ArenaPlayerFlowReadModel(load, movement, combat, loot),
                new ArenaInputFacade(
                    load,
                    combat,
                    loot,
                    (x, y, cancellation) => Task.CompletedTask,
                    (target, cancellation) => Task.CompletedTask,
                    (drop, cancellation) => Task.CompletedTask),
                view);

            presenter.Render();
            yield return null;

            Assert.That(view.Last.Players, Has.Count.EqualTo(1));
            Assert.That(view.Last.Players[0].PositionXMillimeters, Is.EqualTo(1250));
            Assert.That(view.Last.Monster.HitPoints, Is.EqualTo(1600));
            Assert.That(view.Last.Drops, Has.Count.EqualTo(1));
            Assert.That(view.Last.Drops[0].PositionYMillimeters, Is.EqualTo(4000));
            Assert.That(view.Last.AttackTerminalCopy, Is.EqualTo("Attack: OutOfRange"));
            Assert.That(view.Last.LootTerminalCopy, Is.EqualTo("Loot: AlreadyClaimed"));
        }

        private static ArenaGameplayStart GameplayStart()
        {
            return new ArenaGameplayStart(
                7,
                9,
                new[]
                {
                    new BattleParticipant(1, 2, "neo"),
                    new BattleParticipant(2, 3, "trinity")
                });
        }

        private sealed class RecordingArenaView : IArenaView
        {
            public ArenaPresentationSnapshot Last { get; private set; }
            public string LastInputCopy { get; private set; }

            public void Render(ArenaPresentationSnapshot snapshot)
            {
                Last = snapshot;
            }

            public void ShowInputAccepted(string copy)
            {
                LastInputCopy = copy;
            }
        }
    }
}
