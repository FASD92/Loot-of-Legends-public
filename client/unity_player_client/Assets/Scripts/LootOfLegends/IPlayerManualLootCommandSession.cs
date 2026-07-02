using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public interface IPlayerManualLootCommandSession
    {
        string LastError { get; }

        ulong SessionId { get; }

        bool LootResolvedCaptured { get; }

        PlayerLootResolved LootResolved { get; }

        Task<bool> SendSpaceLootIntentAsync();

        Task<bool> CaptureLootResolvedAsync();

        Task<bool> CaptureLootRejectedAsync();

        Task<bool> CaptureInventorySnapshotAsync();
    }
}
