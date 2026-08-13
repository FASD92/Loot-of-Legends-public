using System;
using UnityEngine.SceneManagement;

namespace LootOfLegends.Presentation.FinalResult
{
    public sealed class UnityRoomReturnNavigation : IRoomReturnNavigation
    {
        private const string RoomSceneName = "RoomScene";

        public void ReturnToRoom(ulong roomId)
        {
            if (roomId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(roomId));
            }
            SceneManager.LoadScene(RoomSceneName);
        }
    }
}
