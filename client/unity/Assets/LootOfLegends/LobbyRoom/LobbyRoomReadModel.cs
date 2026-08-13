using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;

namespace LootOfLegends.LobbyRoom
{
    public sealed class LobbyRoomReadModel : ILobbyRoomInboundMessageSink
    {
        private IReadOnlyList<RoomSummary> rooms =
            new ReadOnlyCollection<RoomSummary>(new List<RoomSummary>());
        private long revision;

        public event Action Changed;

        public ulong SessionId { get; private set; }
        public ulong SessionGeneration { get; private set; }
        public string Nickname { get; private set; }
        public IReadOnlyList<RoomSummary> Rooms => rooms;
        public RoomDetailProjection CurrentRoom { get; private set; }
        public bool IsInRoom => CurrentRoom != null;
        public LobbyPresentationSnapshot Lobby { get; private set; } =
            new LobbyPresentationSnapshot(0, string.Empty, Array.Empty<LobbyRoomSummaryView>());
        public RoomPresentationSnapshot Room { get; private set; }

        public bool Apply(LobbyRoomServerMessage message)
        {
            if (message == null)
            {
                throw new ArgumentNullException(nameof(message));
            }
            switch (message)
            {
                case LobbyEntrySnapshot entry:
                    SessionId = entry.SessionId;
                    SessionGeneration = entry.SessionGeneration;
                    Nickname = entry.Nickname;
                    rooms = Copy(entry.Rooms);
                    CurrentRoom = null;
                    Publish();
                    return true;
                case LobbyRoomListUpdate update:
                    rooms = Copy(update.Rooms);
                    // This projection is sent only to LobbyAudience. Receiving it
                    // is the server-origin proof that this session no longer owns
                    // a Room route (for example after voluntary battle leave).
                    CurrentRoom = null;
                    Publish();
                    return true;
                case RoomDetailProjection detail:
                    CurrentRoom = detail;
                    Publish();
                    return true;
                default:
                    return false;
            }
        }

        void ILobbyRoomInboundMessageSink.OnMessage(LobbyRoomServerMessage message)
        {
            Apply(message);
        }

        private static IReadOnlyList<RoomSummary> Copy(IReadOnlyList<RoomSummary> source)
        {
            return new ReadOnlyCollection<RoomSummary>(new List<RoomSummary>(source));
        }

        private void Publish()
        {
            revision++;
            var lobbyRooms = new List<LobbyRoomSummaryView>(rooms.Count);
            foreach (RoomSummary summary in rooms)
            {
                lobbyRooms.Add(new LobbyRoomSummaryView(
                    summary.RoomId,
                    summary.Title,
                    summary.MemberCount,
                    summary.Capacity));
            }
            Lobby = new LobbyPresentationSnapshot(revision, Nickname, lobbyRooms);
            Room = CurrentRoom == null ? null : BuildRoom(CurrentRoom);
            Changed?.Invoke();
        }

        private RoomPresentationSnapshot BuildRoom(RoomDetailProjection detail)
        {
            var members = new List<RoomMemberPresentation>(detail.Members.Count);
            foreach (RoomMember member in detail.Members)
            {
                bool local = member.SessionId == SessionId &&
                    member.SessionGeneration == SessionGeneration;
                bool host = member.SessionId == detail.HostSessionId &&
                    member.SessionGeneration == detail.HostSessionGeneration;
                members.Add(new RoomMemberPresentation(
                    member.SessionId,
                    member.SessionGeneration,
                    member.Nickname,
                    member.Ready,
                    local,
                    host));
            }
            return new RoomPresentationSnapshot(
                revision,
                detail.RoomId,
                detail.Title,
                detail.Capacity,
                members);
        }
    }
}
