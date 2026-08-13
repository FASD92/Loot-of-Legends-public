using System.Collections;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Presentation;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class RudpCombatReadModelTests
    {
        [UnityTest]
        public IEnumerator HpAndOutcomeRenderOnlyAfterServerMessages()
        {
            var readModel = new BattleCombatReadModel(7);
            readModel.Apply(new RudpMonsterSpawned(
                new RudpEventId(1, 1), 7, RudpEventStreamKind.CombatLifecycle,
                1, 1, 0, 0, 1600, 1));
            var gameObject = new GameObject("Authoritative combat projection");
            var presenter = gameObject.AddComponent<BattleCombatPresenter>();
            var input = new AttackInputFacade(
                new RecordingRudpSender(),
                () => new RudpHeader(RudpFlag.Reliable, 1, 2, 3, 9, 8, 0, 27),
                7);

            Task<RudpCommandId> send = input.AttackAsync(1, CancellationToken.None);
            while (!send.IsCompleted)
            {
                yield return null;
            }
            presenter.Render(readModel);
            Assert.That(gameObject.GetComponent<TextMesh>().text, Does.Contain("1600/1600"));

            readModel.Apply(new RudpAttackTerminalResult(
                send.Result, 7, RudpAttackResultCode.Ok, 1, 1580, 1,
                RudpCombatOutcome.None));
            presenter.Render(readModel);
            Assert.That(gameObject.GetComponent<TextMesh>().text, Does.Contain("1580/1600"));

            readModel.Apply(new RudpCombatTerminalEvent(
                new RudpEventId(1, 2), 7, RudpEventStreamKind.CombatLifecycle,
                2, RudpCombatOutcome.MonsterDefeated, 1, 600, 1));
            presenter.Render(readModel);
            Assert.That(gameObject.GetComponent<TextMesh>().text, Does.Contain("MonsterDefeated"));

            Object.Destroy(gameObject);
        }

        private sealed class RecordingRudpSender : IRudpDatagramSender
        {
            public List<byte[]> Datagrams { get; } = new List<byte[]>();

            public Task SendAsync(byte[] datagram, CancellationToken cancellationToken)
            {
                Datagrams.Add(datagram);
                return Task.CompletedTask;
            }
        }
    }
}
