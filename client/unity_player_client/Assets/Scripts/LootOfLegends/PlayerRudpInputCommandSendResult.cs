namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerRudpInputCommandSendResult
    {
        public readonly ulong SessionId;
        public readonly uint Sequence;
        public readonly uint CommandSequence;
        public readonly uint TargetHintMonsterId;
        public readonly string LocalEndpoint;

        public PlayerRudpInputCommandSendResult(
            ulong sessionId,
            uint sequence,
            uint commandSequence,
            uint targetHintMonsterId,
            string localEndpoint)
        {
            SessionId = sessionId;
            Sequence = sequence;
            CommandSequence = commandSequence;
            TargetHintMonsterId = targetHintMonsterId;
            LocalEndpoint = localEndpoint ?? string.Empty;
        }

        public bool IsSent =>
            SessionId != 0UL &&
            Sequence != 0U &&
            CommandSequence != 0U;
    }
}
