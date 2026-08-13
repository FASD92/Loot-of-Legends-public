using UnityEngine.SceneManagement;

namespace LootOfLegends.Presentation.Common
{
    public sealed class UnityRoomNavigation : IRoomNavigation
    {
        public void ReturnToRoom()
        {
            SceneManager.LoadScene("RoomScene");
        }
    }
}
