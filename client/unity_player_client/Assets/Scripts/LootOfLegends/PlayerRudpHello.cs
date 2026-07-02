namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerRudpHello
    {
        public const ushort DefaultClientVersion = 1;

        public readonly ushort ClientVersion;
        public readonly uint ClientId;
        public readonly ulong SessionId;

        public PlayerRudpHello(ushort clientVersion, uint clientId, ulong sessionId)
        {
            ClientVersion = clientVersion;
            ClientId = clientId;
            SessionId = sessionId;
        }

        public bool IsValid => ClientVersion != 0 && ClientId != 0U && SessionId != 0UL;

        public static PlayerRudpHello ForSession(ulong sessionId)
        {
            if (sessionId == 0UL)
            {
                return default;
            }

            uint clientId = unchecked((uint)sessionId);
            if (clientId == 0U)
            {
                clientId = unchecked((uint)(sessionId >> 32));
            }
            if (clientId == 0U)
            {
                clientId = 1U;
            }

            return new PlayerRudpHello(DefaultClientVersion, clientId, sessionId);
        }
    }
}
