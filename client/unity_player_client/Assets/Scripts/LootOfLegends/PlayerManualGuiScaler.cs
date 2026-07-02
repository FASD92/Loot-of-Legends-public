using System;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public static class PlayerManualGuiScaler
    {
        public const float ReferenceHeight = 720.0f;
        public const float MaxScale = 2.0f;
        public const int LabelFontSize = 18;
        public const int ButtonFontSize = 18;
        public const int TextFieldFontSize = 18;
        public const int BoxFontSize = 18;
        public const float LabelFixedHeight = 32.0f;
        public const float ButtonFixedHeight = 44.0f;
        public const float TextFieldFixedHeight = 44.0f;

        public static float CalculateScale(int screenHeight)
        {
            if (screenHeight <= 0)
            {
                return 1.0f;
            }

            return Mathf.Clamp(screenHeight / ReferenceHeight, 1.0f, MaxScale);
        }

        public static Scope Begin(int screenHeight)
        {
            return new Scope(CalculateScale(screenHeight));
        }

        public static Rect CalculateCenteredRect(
            int screenWidth,
            int screenHeight,
            float width,
            float height)
        {
            float scale = CalculateScale(screenHeight);
            float scaledScreenWidth = Mathf.Max(0.0f, screenWidth / scale);
            float scaledScreenHeight = Mathf.Max(0.0f, screenHeight / scale);
            float clampedWidth = Mathf.Min(width, scaledScreenWidth);
            float clampedHeight = Mathf.Min(height, scaledScreenHeight);
            return new Rect(
                Mathf.Max(0.0f, (scaledScreenWidth - clampedWidth) * 0.5f),
                Mathf.Max(0.0f, (scaledScreenHeight - clampedHeight) * 0.5f),
                clampedWidth,
                clampedHeight);
        }

        public readonly struct Scope : IDisposable
        {
            private readonly Matrix4x4 previousMatrix;
            private readonly int previousLabelFontSize;
            private readonly int previousButtonFontSize;
            private readonly int previousTextFieldFontSize;
            private readonly int previousBoxFontSize;
            private readonly float previousLabelFixedHeight;
            private readonly float previousButtonFixedHeight;
            private readonly float previousTextFieldFixedHeight;

            public Scope(float scale)
            {
                previousMatrix = GUI.matrix;
                previousLabelFontSize = GUI.skin.label.fontSize;
                previousButtonFontSize = GUI.skin.button.fontSize;
                previousTextFieldFontSize = GUI.skin.textField.fontSize;
                previousBoxFontSize = GUI.skin.box.fontSize;
                previousLabelFixedHeight = GUI.skin.label.fixedHeight;
                previousButtonFixedHeight = GUI.skin.button.fixedHeight;
                previousTextFieldFixedHeight = GUI.skin.textField.fixedHeight;
                GUI.skin.label.fontSize = LabelFontSize;
                GUI.skin.button.fontSize = ButtonFontSize;
                GUI.skin.textField.fontSize = TextFieldFontSize;
                GUI.skin.box.fontSize = BoxFontSize;
                GUI.skin.label.fixedHeight = LabelFixedHeight;
                GUI.skin.button.fixedHeight = ButtonFixedHeight;
                GUI.skin.textField.fixedHeight = TextFieldFixedHeight;
                if (!Mathf.Approximately(scale, 1.0f))
                {
                    GUI.matrix =
                        Matrix4x4.TRS(
                            Vector3.zero,
                            Quaternion.identity,
                            new Vector3(scale, scale, 1.0f)) *
                        previousMatrix;
                }
            }

            public void Dispose()
            {
                GUI.matrix = previousMatrix;
                GUI.skin.label.fontSize = previousLabelFontSize;
                GUI.skin.button.fontSize = previousButtonFontSize;
                GUI.skin.textField.fontSize = previousTextFieldFontSize;
                GUI.skin.box.fontSize = previousBoxFontSize;
                GUI.skin.label.fixedHeight = previousLabelFixedHeight;
                GUI.skin.button.fixedHeight = previousButtonFixedHeight;
                GUI.skin.textField.fixedHeight = previousTextFieldFixedHeight;
            }
        }
    }
}
