namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerNetworkConnectResult
    {
        public readonly ulong SessionId;
        public readonly PlayerClientListSnapshot ClientListSnapshot;
        public readonly PlayerRoomListSnapshot InitialRoomListSnapshot;

        public PlayerNetworkConnectResult(ulong sessionId)
            : this(sessionId, DefaultSnapshotFor(sessionId), default)
        {
        }

        public PlayerNetworkConnectResult(
            ulong sessionId,
            PlayerClientListSnapshot clientListSnapshot)
            : this(sessionId, clientListSnapshot, default)
        {
        }

        public PlayerNetworkConnectResult(
            ulong sessionId,
            PlayerRoomListSnapshot initialRoomListSnapshot)
            : this(sessionId, DefaultSnapshotFor(sessionId), initialRoomListSnapshot)
        {
        }

        public PlayerNetworkConnectResult(
            ulong sessionId,
            PlayerClientListSnapshot clientListSnapshot,
            PlayerRoomListSnapshot initialRoomListSnapshot)
        {
            SessionId = sessionId;
            ClientListSnapshot = clientListSnapshot;
            InitialRoomListSnapshot = initialRoomListSnapshot;
        }

        public bool IsValid => SessionId != 0UL && IsSelfListed;

        public int ClientSessionCount => ClientListSnapshot.Count;

        public bool IsSelfListed => ClientListSnapshot.ContainsSession(SessionId);

        public ulong[] ClientSessionIds => ClientListSnapshot.ToArray();

        public ulong ClientSessionIdAt(int index)
        {
            return ClientListSnapshot.SessionIdAt(index);
        }

        public int InitialRoomListCount => InitialRoomListSnapshot.Count;

        public PlayerRoomListEntry[] InitialRoomListEntries => InitialRoomListSnapshot.ToArray();

        public PlayerRoomListEntry InitialRoomListEntryAt(int index)
        {
            return InitialRoomListSnapshot.RoomAt(index);
        }

        private static PlayerClientListSnapshot DefaultSnapshotFor(ulong sessionId)
        {
            if (sessionId == 0UL)
            {
                return default;
            }

            return new PlayerClientListSnapshot(new[] { sessionId });
        }
    }
}
