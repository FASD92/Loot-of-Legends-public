namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerRudpStateSnapshotPlayer
    {
        public const float PositionScale = 1000.0f;

        public readonly ulong SessionId;
        public readonly int PosX;
        public readonly int PosY;

        public PlayerRudpStateSnapshotPlayer(ulong sessionId, int posX, int posY)
        {
            SessionId = sessionId;
            PosX = posX;
            PosY = posY;
        }

        public float WorldX => PosX / PositionScale;

        public float WorldZ => PosY / PositionScale;

        public bool IsValid => SessionId != 0UL;
    }

    public readonly struct PlayerRudpStateSnapshot
    {
        private static readonly PlayerRudpStateSnapshotPlayer[] EmptyPlayers =
            new PlayerRudpStateSnapshotPlayer[0];

        public readonly uint RoomId;
        public readonly uint ServerTick;
        private readonly PlayerRudpStateSnapshotPlayer[] players;

        public PlayerRudpStateSnapshot(
            uint roomId,
            uint serverTick,
            PlayerRudpStateSnapshotPlayer[] players)
        {
            RoomId = roomId;
            ServerTick = serverTick;
            this.players = players ?? EmptyPlayers;
        }

        public bool IsValid => RoomId != 0U;

        public int Count => players != null ? players.Length : 0;

        public PlayerRudpStateSnapshotPlayer[] Players =>
            players != null ? (PlayerRudpStateSnapshotPlayer[])players.Clone() : EmptyPlayers;

        public bool TryGetPlayer(
            ulong sessionId,
            out PlayerRudpStateSnapshotPlayer player)
        {
            if (players != null)
            {
                for (int index = 0; index < players.Length; ++index)
                {
                    if (players[index].SessionId == sessionId)
                    {
                        player = players[index];
                        return true;
                    }
                }
            }

            player = default;
            return false;
        }
    }
}
