using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public interface IPlayerMoveIntentSender
    {
        Task<bool> SendMoveIntentAsync(PlayerInputIntent intent);
    }
}
