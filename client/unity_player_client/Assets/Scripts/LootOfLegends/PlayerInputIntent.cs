namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerInputIntent
    {
        public readonly short MoveX;
        public readonly short MoveZ;
        public readonly bool SpaceLootPressed;

        private PlayerInputIntent(short moveX, short moveZ, bool spaceLootPressed)
        {
            MoveX = moveX;
            MoveZ = moveZ;
            SpaceLootPressed = spaceLootPressed;
        }

        public static PlayerInputIntent FromKeyboard(
            float horizontal,
            float vertical,
            bool spaceLootPressed)
        {
            return new PlayerInputIntent(
                QuantizeAxis(horizontal),
                QuantizeAxis(vertical),
                spaceLootPressed);
        }

        private static short QuantizeAxis(float value)
        {
            if (value > 0.0f)
            {
                return 1;
            }

            if (value < 0.0f)
            {
                return -1;
            }

            return 0;
        }
    }
}
