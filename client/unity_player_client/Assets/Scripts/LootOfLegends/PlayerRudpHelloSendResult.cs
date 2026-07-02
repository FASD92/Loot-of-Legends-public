namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerRudpHelloSendResult
    {
        public readonly ulong SessionId;
        public readonly uint Sequence;
        public readonly string LocalEndpoint;

        public PlayerRudpHelloSendResult(ulong sessionId, uint sequence, string localEndpoint)
        {
            SessionId = sessionId;
            Sequence = sequence;
            LocalEndpoint = localEndpoint ?? string.Empty;
        }

        public bool IsSent => SessionId != 0UL && Sequence != 0U;
    }
}
