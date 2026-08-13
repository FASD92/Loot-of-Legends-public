using System.Text;
using LootOfLegends.LobbyRoom;
using UnityEngine;

namespace LootOfLegends.Presentation.Lobby
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class LobbyTextView : MonoBehaviour, ILobbyView
    {
        private TextMesh label;
        private string commandCopy = string.Empty;

        private void Awake()
        {
            label = GetComponent<TextMesh>();
            if (label == null)
            {
                label = gameObject.AddComponent<TextMesh>();
            }
            label.text = "Waiting for server Lobby projection";
        }

        public void Render(LobbyPresentationSnapshot snapshot)
        {
            var copy = new StringBuilder();
            copy.AppendLine("LOBBY — " + snapshot.Nickname);
            foreach (LobbyRoomSummaryView room in snapshot.Rooms)
            {
                copy.Append(room.RoomId).Append("  ")
                    .Append(room.Title).Append("  ")
                    .Append(room.MemberCount).Append('/').Append(room.Capacity)
                    .AppendLine(room.IsFull ? "  FULL" : string.Empty);
            }
            copy.Append(commandCopy);
            label.text = copy.ToString();
        }

        public void ShowCommandResult(RoomCommandResult result)
        {
            commandCopy = result == RoomCommandResult.Ok
                ? "Request accepted — waiting for server projection"
                : "Request rejected: " + result;
        }
    }
}
