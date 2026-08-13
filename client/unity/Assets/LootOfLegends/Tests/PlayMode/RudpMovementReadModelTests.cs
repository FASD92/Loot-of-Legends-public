using System.Collections;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class RudpMovementReadModelTests
    {
        [UnityTest]
        public IEnumerator LatestAuthoritativePositionRendersWithoutClientPrediction()
        {
            var readModel = new BattleMovementReadModel(7);
            var player = new GameObject("Authoritative player projection");

            Assert.That(
                readModel.Apply(new RudpStateSnapshot(
                    7,
                    1,
                    2,
                    new[] { new RudpSnapshotPlayer(1, 1250, -1750) })),
                Is.True);
            PlayerPosition position = readModel.Positions[1];
            player.transform.position = new Vector3(
                position.PositionXMillimeters / 1000f,
                0,
                position.PositionYMillimeters / 1000f);
            yield return null;

            Assert.That(player.transform.position, Is.EqualTo(new Vector3(1.25f, 0, -1.75f)));
            Assert.That(
                typeof(BattleMovementReadModel).GetProperty("PredictedPosition"),
                Is.Null);
            Object.Destroy(player);
        }
    }
}
