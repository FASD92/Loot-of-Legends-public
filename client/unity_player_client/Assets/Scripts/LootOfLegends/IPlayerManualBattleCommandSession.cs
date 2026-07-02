using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public interface IPlayerManualBattleCommandSession
    {
        Task<bool> CaptureBattleStartAsync();

        Task<bool> CaptureMonsterSpawnAsync();
    }
}
