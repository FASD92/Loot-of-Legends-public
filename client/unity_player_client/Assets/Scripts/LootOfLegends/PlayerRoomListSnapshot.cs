using System;

namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerRoomListSnapshot
    {
        private readonly PlayerRoomListEntry[] rooms;

        public PlayerRoomListSnapshot(PlayerRoomListEntry[] rooms)
        {
            if (rooms == null || rooms.Length == 0)
            {
                this.rooms = Array.Empty<PlayerRoomListEntry>();
                return;
            }

            this.rooms = new PlayerRoomListEntry[rooms.Length];
            Array.Copy(rooms, this.rooms, rooms.Length);
        }

        public int Count => rooms != null ? rooms.Length : 0;

        public bool ContainsRoom(uint roomId)
        {
            if (roomId == 0U)
            {
                return false;
            }

            for (int index = 0; index < Count; ++index)
            {
                if (rooms[index].RoomId == roomId)
                {
                    return true;
                }
            }

            return false;
        }

        public PlayerRoomListEntry RoomAt(int index)
        {
            if (index < 0 || index >= Count)
            {
                throw new ArgumentOutOfRangeException(nameof(index));
            }

            return rooms[index];
        }

        public PlayerRoomListEntry[] ToArray()
        {
            if (Count == 0)
            {
                return Array.Empty<PlayerRoomListEntry>();
            }

            PlayerRoomListEntry[] copy = new PlayerRoomListEntry[Count];
            Array.Copy(rooms, copy, Count);
            return copy;
        }
    }
}
