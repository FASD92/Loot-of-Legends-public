using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;

namespace LootOfLegends.LobbyRoom
{
    public sealed class LobbyRoomSummaryView
    {
        public LobbyRoomSummaryView(ulong roomId, string title, byte memberCount, byte capacity)
        {
            RoomId = roomId;
            Title = title;
            MemberCount = memberCount;
            Capacity = capacity;
        }

        public ulong RoomId { get; }
        public string Title { get; }
        public byte MemberCount { get; }
        public byte Capacity { get; }
        public bool IsFull => MemberCount >= Capacity;
    }

    public sealed class LobbyPresentationSnapshot
    {
        private readonly IReadOnlyList<LobbyRoomSummaryView> rooms;

        public LobbyPresentationSnapshot(
            long revision,
            string nickname,
            IReadOnlyList<LobbyRoomSummaryView> rooms)
        {
            Revision = revision;
            Nickname = nickname ?? string.Empty;
            this.rooms = new ReadOnlyCollection<LobbyRoomSummaryView>(
                new List<LobbyRoomSummaryView>(rooms ??
                    throw new ArgumentNullException(nameof(rooms))));
        }

        public long Revision { get; }
        public string Nickname { get; }
        public IReadOnlyList<LobbyRoomSummaryView> Rooms => rooms;
    }

    public sealed class RoomMemberPresentation
    {
        public RoomMemberPresentation(
            ulong sessionId,
            ulong sessionGeneration,
            string nickname,
            bool ready,
            bool isLocal,
            bool isHost)
        {
            SessionId = sessionId;
            SessionGeneration = sessionGeneration;
            Nickname = nickname;
            Ready = ready;
            IsLocal = isLocal;
            IsHost = isHost;
        }

        public ulong SessionId { get; }
        public ulong SessionGeneration { get; }
        public string Nickname { get; }
        public bool Ready { get; }
        public bool IsLocal { get; }
        public bool IsHost { get; }
    }

    public sealed class RoomPresentationSnapshot
    {
        private readonly IReadOnlyList<RoomMemberPresentation> members;

        public RoomPresentationSnapshot(
            long revision,
            ulong roomId,
            string title,
            byte capacity,
            IReadOnlyList<RoomMemberPresentation> members)
        {
            Revision = revision;
            RoomId = roomId;
            Title = title;
            Capacity = capacity;
            this.members = new ReadOnlyCollection<RoomMemberPresentation>(
                new List<RoomMemberPresentation>(members ??
                    throw new ArgumentNullException(nameof(members))));
            IsLocalHost = this.members.Any(member => member.IsLocal && member.IsHost);
            CanToggleReady = this.members.Any(member => member.IsLocal);
            CanStart = IsLocalHost && this.members.Count >= 2 &&
                this.members.All(member => member.Ready);
        }

        public long Revision { get; }
        public ulong RoomId { get; }
        public string Title { get; }
        public byte Capacity { get; }
        public IReadOnlyList<RoomMemberPresentation> Members => members;
        public bool IsLocalHost { get; }
        public bool CanToggleReady { get; }
        public bool CanStart { get; }
    }
}
