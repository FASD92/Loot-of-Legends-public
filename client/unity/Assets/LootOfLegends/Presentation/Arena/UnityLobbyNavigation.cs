using UnityEngine.SceneManagement;
using LootOfLegends.Presentation.Common;

namespace LootOfLegends.Presentation.Arena
{
    public sealed class UnityLobbyNavigation :
        ILobbyNavigation,
        IBattleRecoveryLobbyNavigation
    {
        public void ReturnToLobby()
        {
            SceneManager.LoadScene("LobbyScene");
        }
    }
}
