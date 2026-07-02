using System;

namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerInventorySnapshot
    {
        private readonly PlayerInventoryEntry[] entries;

        public PlayerInventorySnapshot(
            ulong sessionId,
            ushort currentWeight,
            ushort maxWeight,
            PlayerInventoryEntry[] entries)
        {
            SessionId = sessionId;
            CurrentWeight = currentWeight;
            MaxWeight = maxWeight;

            if (entries == null || entries.Length == 0)
            {
                this.entries = Array.Empty<PlayerInventoryEntry>();
                return;
            }

            this.entries = new PlayerInventoryEntry[entries.Length];
            Array.Copy(entries, this.entries, entries.Length);
        }

        public ulong SessionId { get; }

        public ushort CurrentWeight { get; }

        public ushort MaxWeight { get; }

        public int Count => entries != null ? entries.Length : 0;

        public PlayerInventoryEntry EntryAt(int index)
        {
            if (index < 0 || index >= Count)
            {
                throw new ArgumentOutOfRangeException(nameof(index));
            }

            return entries[index];
        }

        public PlayerInventoryEntry[] ToArray()
        {
            if (Count == 0)
            {
                return Array.Empty<PlayerInventoryEntry>();
            }

            PlayerInventoryEntry[] copy = new PlayerInventoryEntry[Count];
            Array.Copy(entries, copy, Count);
            return copy;
        }
    }
}
