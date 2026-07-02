namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerLootResolved
    {
        public readonly uint RoomId;
        public readonly uint DropId;
        public readonly ulong WinnerSessionId;
        public readonly uint ItemId;
        public readonly ushort Quantity;

        public PlayerLootResolved(
            uint roomId,
            uint dropId,
            ulong winnerSessionId,
            uint itemId,
            ushort quantity)
        {
            RoomId = roomId;
            DropId = dropId;
            WinnerSessionId = winnerSessionId;
            ItemId = itemId;
            Quantity = quantity;
        }
    }
}
