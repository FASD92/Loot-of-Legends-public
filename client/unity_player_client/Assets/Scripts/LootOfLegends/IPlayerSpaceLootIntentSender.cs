using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public interface IPlayerSpaceLootIntentSender
    {
        Task<bool> SendSpaceLootIntentAsync();
    }
}
