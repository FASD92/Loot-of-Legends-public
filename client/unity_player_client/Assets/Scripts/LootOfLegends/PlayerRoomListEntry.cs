namespace LootOfLegends.PlayerClient
{
    public enum PlayerRoomStatus : byte
    {
        Open = 0,
        InProgress = 1
    }

    public enum PlayerLobbyReturnReason : byte
    {
        None = 0,
        ArenaLoadTimeout = 1,
        ArenaLoadMinimumFailure = 2,
        HostKick = 3,
        ResultGenerationFailure = 4
    }

    public readonly struct PlayerRoomListEntry
    {
        public readonly uint RoomId;
        public readonly ushort PlayerCount;
        public readonly ushort MaxPlayers;
        public readonly PlayerRoomStatus Status;
        public readonly string Title;

        public PlayerRoomListEntry(
            uint roomId,
            ushort playerCount,
            ushort maxPlayers,
            PlayerRoomStatus status = PlayerRoomStatus.Open,
            string title = "")
        {
            RoomId = roomId;
            PlayerCount = playerCount;
            MaxPlayers = maxPlayers;
            Status = status;
            Title = title ?? string.Empty;
        }
    }

    public readonly struct PlayerRoomMemberEntry
    {
        public readonly ulong SessionId;
        public readonly string Nickname;
        public readonly bool Ready;

        public PlayerRoomMemberEntry(ulong sessionId, string nickname, bool ready)
        {
            SessionId = sessionId;
            Nickname = nickname ?? string.Empty;
            Ready = ready;
        }
    }

    public readonly struct PlayerRoomTargetActionEntry
    {
        public readonly ulong TargetSessionId;
        public readonly ushort TargetActionMask;

        public PlayerRoomTargetActionEntry(ulong targetSessionId, ushort targetActionMask)
        {
            TargetSessionId = targetSessionId;
            TargetActionMask = targetActionMask;
        }
    }

    public readonly struct PlayerRoomDetailState
    {
        private readonly PlayerRoomMemberEntry[] members;
        private readonly PlayerRoomTargetActionEntry[] targetActions;

        public PlayerRoomDetailState(
            uint roomId,
            PlayerRoomStatus status,
            string title,
            byte maxPlayers,
            PlayerRoomMemberEntry[] members,
            ushort selfActionMask,
            PlayerRoomTargetActionEntry[] targetActions)
        {
            RoomId = roomId;
            Status = status;
            Title = title ?? string.Empty;
            MaxPlayers = maxPlayers;
            SelfActionMask = selfActionMask;

            if (members == null || members.Length == 0)
            {
                this.members = System.Array.Empty<PlayerRoomMemberEntry>();
            }
            else
            {
                this.members = new PlayerRoomMemberEntry[members.Length];
                System.Array.Copy(members, this.members, members.Length);
            }

            if (targetActions == null || targetActions.Length == 0)
            {
                this.targetActions = System.Array.Empty<PlayerRoomTargetActionEntry>();
            }
            else
            {
                this.targetActions = new PlayerRoomTargetActionEntry[targetActions.Length];
                System.Array.Copy(targetActions, this.targetActions, targetActions.Length);
            }
        }

        public uint RoomId { get; }

        public PlayerRoomStatus Status { get; }

        public string Title { get; }

        public byte MaxPlayers { get; }

        public ushort SelfActionMask { get; }

        public int MemberCount => members != null ? members.Length : 0;

        public int TargetActionCount => targetActions != null ? targetActions.Length : 0;

        public PlayerRoomMemberEntry MemberAt(int index)
        {
            if (index < 0 || index >= MemberCount)
            {
                throw new System.ArgumentOutOfRangeException(nameof(index));
            }

            return members[index];
        }

        public PlayerRoomTargetActionEntry TargetActionAt(int index)
        {
            if (index < 0 || index >= TargetActionCount)
            {
                throw new System.ArgumentOutOfRangeException(nameof(index));
            }

            return targetActions[index];
        }

        public PlayerRoomMemberEntry[] MembersToArray()
        {
            if (MemberCount == 0)
            {
                return System.Array.Empty<PlayerRoomMemberEntry>();
            }

            PlayerRoomMemberEntry[] copy = new PlayerRoomMemberEntry[MemberCount];
            System.Array.Copy(members, copy, MemberCount);
            return copy;
        }

        public PlayerRoomTargetActionEntry[] TargetActionsToArray()
        {
            if (TargetActionCount == 0)
            {
                return System.Array.Empty<PlayerRoomTargetActionEntry>();
            }

            PlayerRoomTargetActionEntry[] copy =
                new PlayerRoomTargetActionEntry[TargetActionCount];
            System.Array.Copy(targetActions, copy, TargetActionCount);
            return copy;
        }
    }
}
