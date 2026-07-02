namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerDropEntryV2
    {
        public readonly uint DropId;
        public readonly uint ItemId;
        public readonly ushort Quantity;
        public readonly int PosX;
        public readonly int PosY;

        public PlayerDropEntryV2(
            uint dropId,
            uint itemId,
            ushort quantity,
            int posX,
            int posY)
        {
            DropId = dropId;
            ItemId = itemId;
            Quantity = quantity;
            PosX = posX;
            PosY = posY;
        }
    }
}
