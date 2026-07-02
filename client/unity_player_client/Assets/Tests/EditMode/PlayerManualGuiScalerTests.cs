using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerManualGuiScalerTests
    {
        [Test]
        public void CalculateScaleUsesReadableSizeForHighResolutionDisplays()
        {
            Assert.AreEqual(1.0f, PlayerManualGuiScaler.CalculateScale(720));
            Assert.AreEqual(1.5f, PlayerManualGuiScaler.CalculateScale(1080));
            Assert.AreEqual(2.0f, PlayerManualGuiScaler.CalculateScale(1440));
        }

        [Test]
        public void CalculateScaleClampsSmallAndVeryLargeDisplays()
        {
            Assert.AreEqual(1.0f, PlayerManualGuiScaler.CalculateScale(0));
            Assert.AreEqual(1.0f, PlayerManualGuiScaler.CalculateScale(600));
            Assert.AreEqual(2.0f, PlayerManualGuiScaler.CalculateScale(2160));
        }

        [Test]
        public void ExposesReadableDefaultFontSizes()
        {
            Assert.GreaterOrEqual(PlayerManualGuiScaler.LabelFontSize, 18);
            Assert.GreaterOrEqual(PlayerManualGuiScaler.ButtonFontSize, 18);
            Assert.GreaterOrEqual(PlayerManualGuiScaler.TextFieldFontSize, 18);
        }

        [Test]
        public void ExposesReadableControlHeights()
        {
            Assert.GreaterOrEqual(
                PlayerManualGuiScaler.ButtonFixedHeight,
                PlayerManualGuiScaler.ButtonFontSize + 24.0f);
            Assert.GreaterOrEqual(
                PlayerManualGuiScaler.TextFieldFixedHeight,
                PlayerManualGuiScaler.TextFieldFontSize + 24.0f);
            Assert.GreaterOrEqual(
                PlayerManualGuiScaler.LabelFixedHeight,
                PlayerManualGuiScaler.LabelFontSize + 12.0f);
        }

        [Test]
        public void CalculateCenteredRectCentersAndClampsToScaledScreen()
        {
            UnityEngine.Rect desktop =
                PlayerManualGuiScaler.CalculateCenteredRect(1280, 720, 520, 440);
            Assert.AreEqual(380.0f, desktop.x);
            Assert.AreEqual(140.0f, desktop.y);
            Assert.AreEqual(520.0f, desktop.width);
            Assert.AreEqual(440.0f, desktop.height);

            UnityEngine.Rect small =
                PlayerManualGuiScaler.CalculateCenteredRect(640, 360, 520, 440);
            Assert.GreaterOrEqual(small.x, 0.0f);
            Assert.GreaterOrEqual(small.y, 0.0f);
            Assert.LessOrEqual(small.x + small.width, 640.0f);
            Assert.LessOrEqual(small.y + small.height, 360.0f);
        }
    }
}
