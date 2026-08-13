using System.Text;
using LootOfLegends.LobbyRoom;
using UnityEngine;

namespace LootOfLegends.Presentation.Room
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class RoomTextView : MonoBehaviour, IRoomView
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
            label.text = "Waiting for server Room projection";
        }

        public void Render(RoomPresentationSnapshot snapshot)
        {
            if (snapshot == null)
            {
                label.text = "Waiting for server Room projection";
                return;
            }
            var copy = new StringBuilder();
            copy.Append(snapshot.Title).Append("  ")
                .Append(snapshot.Members.Count).Append('/').Append(snapshot.Capacity)
                .AppendLine();
            foreach (RoomMemberPresentation member in snapshot.Members)
            {
                copy.Append(member.IsHost ? "HOST " : "     ")
                    .Append(member.Nickname)
                    .AppendLine(member.Ready ? "  READY" : "  UNREADY");
            }
            copy.AppendLine(snapshot.CanStart ? "START AVAILABLE" : "WAITING FOR READY");
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
