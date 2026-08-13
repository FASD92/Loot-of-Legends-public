using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Presentation;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class RudpLootReadModelTests
    {
        [UnityTest]
        public IEnumerator MarkerAndOwnershipChangeOnlyFromServerProjection()
        {
            var readModel = new BattleLootReadModel(7);
            readModel.Apply(new RudpDropStateSnapshot(
                7,
                1,
                RudpLootResolutionState.Open,
                new[]
                {
                    new RudpLootDropProjection(
                        1, 2, 1, 1000, -2000,
                        RudpLootDropState.Available, 0)
                }));
            var gameObject = new GameObject("Server loot marker");
            var presenter = gameObject.AddComponent<BattleLootMarkerPresenter>();
            presenter.Render(readModel.Drops.Single());
            Assert.That(gameObject.transform.position.x, Is.EqualTo(1f));
            Assert.That(gameObject.transform.position.z, Is.EqualTo(-2f));
            Assert.That(gameObject.GetComponent<TextMesh>().text, Does.Contain("Available"));

            var sender = new RecordingRudpSender();
            var input = new ClaimLootInputFacade(
                sender,
                () => new RudpHeader(RudpFlag.Reliable, 1, 2, 3, 9, 8, 15, 32),
                7);
            Task<RudpCommandId> send = input.ClaimAsync(1, CancellationToken.None);
            while (!send.IsCompleted)
            {
                yield return null;
            }
            readModel.Apply(new RudpClaimLootTerminalResult(
                send.Result, 7, 1, RudpClaimLootResultCode.Ok));
            presenter.Render(readModel.Drops.Single());
            Assert.That(gameObject.GetComponent<TextMesh>().text, Does.Contain("Available"));

            readModel.Apply(new RudpDropStateSnapshot(
                7,
                2,
                RudpLootResolutionState.Resolved,
                new[]
                {
                    new RudpLootDropProjection(
                        1, 2, 1, 1000, -2000,
                        RudpLootDropState.Claimed, 1)
                }));
            presenter.Render(readModel.Drops.Single());
            Assert.That(gameObject.GetComponent<TextMesh>().text, Does.Contain("Claimed"));
            Assert.That(readModel.Drops.Single().OwnerSessionId, Is.EqualTo(1));

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
