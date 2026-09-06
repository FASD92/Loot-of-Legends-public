using System;
using UnityEngine;

namespace LootOfLegends.Presentation.Arena
{
    public enum ArenaFacing
    {
        Down,
        Up,
        Left,
        Right
    }

    public sealed class ArenaPlayerAppearanceSet : ScriptableObject
    {
        public const int AppearanceCount = 10;
        private const int DirectionCount = 4;
        private const int FramesPerDirection = 4;

        [SerializeField] private Sprite[] frames = Array.Empty<Sprite>();

        public Sprite Resolve(int appearanceSlot, ArenaFacing facing, int frame)
        {
            if (appearanceSlot < 0 || appearanceSlot >= AppearanceCount)
            {
                throw new ArgumentOutOfRangeException(nameof(appearanceSlot));
            }
            if ((int)facing < 0 || (int)facing >= DirectionCount)
            {
                throw new ArgumentOutOfRangeException(nameof(facing));
            }
            if (frame < 0 || frame >= FramesPerDirection)
            {
                throw new ArgumentOutOfRangeException(nameof(frame));
            }
            if (frames == null ||
                frames.Length != AppearanceCount * DirectionCount * FramesPerDirection)
            {
                throw new InvalidOperationException(
                    "Arena player appearance set must contain exactly 160 sprites");
            }

            int index = appearanceSlot * DirectionCount * FramesPerDirection +
                (int)facing * FramesPerDirection + frame;
            return frames[index] != null
                ? frames[index]
                : throw new InvalidOperationException(
                    $"Arena player appearance sprite {index} is missing");
        }
    }
}
