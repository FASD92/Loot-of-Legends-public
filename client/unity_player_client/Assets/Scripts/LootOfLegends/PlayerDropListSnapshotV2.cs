using System;

namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerDropListSnapshotV2
    {
        private readonly PlayerDropEntryV2[] drops;

        public PlayerDropListSnapshotV2(
            uint roomId,
            uint scatterSeed,
            PlayerDropEntryV2[] drops)
        {
            RoomId = roomId;
            ScatterSeed = scatterSeed;

            if (drops == null || drops.Length == 0)
            {
                this.drops = Array.Empty<PlayerDropEntryV2>();
                return;
            }

            this.drops = new PlayerDropEntryV2[drops.Length];
            Array.Copy(drops, this.drops, drops.Length);
        }

        public uint RoomId { get; }

        public uint ScatterSeed { get; }

        public int Count => drops != null ? drops.Length : 0;

        public PlayerDropEntryV2 DropAt(int index)
        {
            if (index < 0 || index >= Count)
            {
                throw new ArgumentOutOfRangeException(nameof(index));
            }

            return drops[index];
        }

        public PlayerDropEntryV2[] ToArray()
        {
            if (Count == 0)
            {
                return Array.Empty<PlayerDropEntryV2>();
            }

            PlayerDropEntryV2[] copy = new PlayerDropEntryV2[Count];
            Array.Copy(drops, copy, Count);
            return copy;
        }
    }
}
