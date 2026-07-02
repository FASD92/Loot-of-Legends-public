using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public static class PlayerLocalMovement
    {
        public const float DefaultSpeedUnitsPerSecond = 1.0f;
        public const float DefaultArenaClampHalfExtent = 50.0f;

        public static Vector3 Apply(
            Vector3 current,
            PlayerInputIntent intent,
            float speedUnitsPerSecond,
            float deltaSeconds,
            float arenaClampHalfExtent)
        {
            Vector3 direction = new Vector3(intent.MoveX, 0.0f, intent.MoveZ);
            if (direction.sqrMagnitude > 1.0f)
            {
                direction.Normalize();
            }

            Vector3 next = current + direction * speedUnitsPerSecond * deltaSeconds;
            next.x = Mathf.Clamp(next.x, -arenaClampHalfExtent, arenaClampHalfExtent);
            next.z = Mathf.Clamp(next.z, -arenaClampHalfExtent, arenaClampHalfExtent);
            next.y = current.y;
            return next;
        }
    }
}
