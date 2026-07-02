using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public interface IPlayerManualCombatCommandSession
    {
        Task<bool> SendAttackIntentAsync();

        Task<bool> CaptureMonsterHealthSnapshotAsync();

        Task<bool> CaptureMonsterDeathAsync();

        Task<bool> CaptureDropListSnapshotV2Async();
    }
}
