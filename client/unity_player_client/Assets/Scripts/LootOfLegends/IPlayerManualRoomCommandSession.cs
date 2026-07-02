using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public interface IPlayerManualRoomCommandSession
    {
        PlayerNetworkSessionStatus Status { get; }

        string LastError { get; }

        ulong SessionId { get; }

        uint CurrentRoomId { get; }

        PlayerRoomListEntry[] RoomListEntries { get; }

        Task ConnectAsync();

        Task<bool> RequestCreateRoomAsync();

        Task<bool> RequestCreateRoomAsync(string roomTitle, int maxPlayers);

        Task<bool> CaptureRoomListSnapshotAsync();

        Task<bool> RequestJoinRoomAsync(uint roomId);

        Task<bool> RequestReadyRoomAsync();

        Task<bool> RequestUnreadyRoomAsync();

        Task<bool> RequestHostStartBattleAsync();

        Task<bool> RequestLeaveRoomAsync();
    }
}
