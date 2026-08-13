using System.Text;
using LootOfLegends.Battle;
using UnityEngine;

namespace LootOfLegends.Presentation.Arena
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class ArenaTextView : MonoBehaviour, IArenaView
    {
        private TextMesh label;
        private string inputCopy = string.Empty;

        private void Awake()
        {
            label = GetComponent<TextMesh>();
            if (label == null)
            {
                label = gameObject.AddComponent<TextMesh>();
            }
            label.text = "Waiting for server ArenaLoadEntry";
        }

        public void Render(ArenaPresentationSnapshot snapshot)
        {
            var copy = new StringBuilder();
            copy.AppendLine(snapshot.WaitingForGameplayStart
                ? "WAITING FOR SERVER GAMEPLAY START"
                : snapshot.ControlsEnabled ? "GAMEPLAY ACTIVE" : "ARENA IDLE");
            foreach (ArenaPlayerProjection player in snapshot.Players)
            {
                copy.Append("P").Append(player.SessionId).Append("  ")
                    .Append(player.PositionXMillimeters).Append(", ")
                    .Append(player.PositionYMillimeters).AppendLine();
            }
            if (snapshot.Monster.HasMonster)
            {
                copy.Append("MONSTER ").Append(snapshot.Monster.HitPoints).Append('/')
                    .Append(snapshot.Monster.MaximumHitPoints).Append("  ")
                    .AppendLine(snapshot.Monster.Outcome);
            }
            foreach (ArenaDropProjection drop in snapshot.Drops)
            {
                copy.Append("DROP ").Append(drop.DropId).Append(" x")
                    .Append(drop.Quantity).Append("  ").AppendLine(drop.State);
            }
            copy.AppendLine(snapshot.AttackTerminalCopy);
            copy.AppendLine(snapshot.LootTerminalCopy);
            copy.Append(inputCopy);
            label.text = copy.ToString();
        }

        public void ShowInputAccepted(string copy)
        {
            inputCopy = copy ?? string.Empty;
        }
    }
}
