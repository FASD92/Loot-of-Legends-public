using System.Text;
using LootOfLegends.Battle;
using UnityEngine;

namespace LootOfLegends.Presentation.FinalResult
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class FinalResultTextView : MonoBehaviour, IFinalResultView
    {
        private TextMesh label;

        private void Awake()
        {
            label = GetComponent<TextMesh>();
            if (label == null)
            {
                label = gameObject.AddComponent<TextMesh>();
            }
        }

        public void Show(FinalResultPresentationSnapshot snapshot)
        {
            gameObject.SetActive(true);
            var copy = new StringBuilder();
            copy.AppendLine(snapshot.HasWinner ? "FINAL RESULT" : "NO WINNER");
            copy.AppendLine(snapshot.Outcome.ToString());
            foreach (FinalResultPresentationRow row in snapshot.Rows)
            {
                copy.Append(row.Rank.HasValue ? row.Rank.Value.ToString() : "-")
                    .Append("  ").Append(row.Nickname)
                    .Append("  ").Append(row.FinalAssetValue);
                if (row.IsTop)
                {
                    copy.Append("  TOP");
                }
                copy.AppendLine();
            }
            label.text = copy.ToString();
        }

        public void Hide()
        {
            gameObject.SetActive(false);
        }
    }
}
