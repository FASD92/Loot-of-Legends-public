using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerLocalMovementTests
    {
        [Test]
        public void ApplyNormalizesDiagonalMovementAndPreservesY()
        {
            Vector3 current = new Vector3(0.0f, 1.1f, 0.0f);
            PlayerInputIntent intent = PlayerInputIntent.FromKeyboard(1.0f, 1.0f, false);

            Vector3 next = PlayerLocalMovement.Apply(current, intent, 5.0f, 1.0f, 9.0f);

            Vector2 displacement = new Vector2(next.x - current.x, next.z - current.z);
            Assert.AreEqual(5.0f, displacement.magnitude, 0.0001f);
            Assert.AreEqual(1.1f, next.y, 0.0001f);
        }

        [Test]
        public void ApplyClampsMovementToArenaBounds()
        {
            Vector3 current = new Vector3(8.5f, 1.1f, -8.75f);
            PlayerInputIntent intent = PlayerInputIntent.FromKeyboard(1.0f, -1.0f, false);

            Vector3 next = PlayerLocalMovement.Apply(current, intent, 5.0f, 1.0f, 9.0f);

            Assert.AreEqual(9.0f, next.x, 0.0001f);
            Assert.AreEqual(-9.0f, next.z, 0.0001f);
            Assert.AreEqual(1.1f, next.y, 0.0001f);
        }

        [Test]
        public void ApplyKeepsZeroIntentInPlace()
        {
            Vector3 current = new Vector3(2.0f, 1.1f, -3.0f);
            PlayerInputIntent intent = PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false);

            Vector3 next = PlayerLocalMovement.Apply(current, intent, 5.0f, 1.0f, 9.0f);

            Assert.AreEqual(current.x, next.x, 0.0001f);
            Assert.AreEqual(current.y, next.y, 0.0001f);
            Assert.AreEqual(current.z, next.z, 0.0001f);
        }
    }
}
