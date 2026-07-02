using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerInputIntentTests
    {
        [Test]
        public void FromKeyboardQuantizesMovementAxes()
        {
            PlayerInputIntent intent = PlayerInputIntent.FromKeyboard(0.75f, -0.2f, false);

            Assert.AreEqual(1, intent.MoveX);
            Assert.AreEqual(-1, intent.MoveZ);
            Assert.False(intent.SpaceLootPressed);
        }

        [Test]
        public void FromKeyboardKeepsZeroMovementAndSpaceLootIntent()
        {
            PlayerInputIntent intent = PlayerInputIntent.FromKeyboard(0.0f, 0.0f, true);

            Assert.AreEqual(0, intent.MoveX);
            Assert.AreEqual(0, intent.MoveZ);
            Assert.True(intent.SpaceLootPressed);
        }
    }
}
