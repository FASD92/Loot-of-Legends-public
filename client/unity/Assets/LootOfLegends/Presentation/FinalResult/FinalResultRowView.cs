using System;
using LootOfLegends.Battle;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.FinalResult
{
    public sealed class FinalResultRowView : MonoBehaviour
    {
        private static readonly Color TopTint = new Color32(255, 220, 125, 110);

        [SerializeField] private Image background;
        [SerializeField] private Text topLabel;
        [SerializeField] private Text rankLabel;
        [SerializeField] private Text nicknameLabel;
        [SerializeField] private Text assetValueLabel;

        public void Render(
            FinalResultPresentationRow result,
            bool isLocal = false,
            bool isHost = false)
        {
            if (result == null)
            {
                throw new ArgumentNullException(nameof(result));
            }

            background.color = TopTint;
            background.enabled = result.IsTop;
            topLabel.text = "최고";
            topLabel.gameObject.SetActive(result.IsTop);
            rankLabel.text = result.Rank.HasValue
                ? result.Rank.Value + "위"
                : "—";
            string marker = isLocal && isHost
                ? " (나·방장)"
                : isLocal
                    ? " (나)"
                    : isHost
                        ? " (방장)"
                        : string.Empty;
            nicknameLabel.text = result.Nickname + marker;
            assetValueLabel.text = result.FinalAssetValue.ToString();
        }
    }
}
