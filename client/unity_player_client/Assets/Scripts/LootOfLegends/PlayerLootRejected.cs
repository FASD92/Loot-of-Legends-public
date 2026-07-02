namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerLootRejected
    {
        public readonly uint RoomId;
        public readonly uint DropId;
        public readonly PlayerLootRejectReason Reason;

        public PlayerLootRejected(
            uint roomId,
            uint dropId,
            PlayerLootRejectReason reason)
        {
            RoomId = roomId;
            DropId = dropId;
            Reason = reason;
        }
    }
}
