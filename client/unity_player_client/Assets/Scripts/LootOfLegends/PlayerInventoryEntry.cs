namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerInventoryEntry
    {
        public readonly uint ItemId;
        public readonly ushort Quantity;

        public PlayerInventoryEntry(uint itemId, ushort quantity)
        {
            ItemId = itemId;
            Quantity = quantity;
        }
    }
}
