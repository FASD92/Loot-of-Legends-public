using LootOfLegends.Battle;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation
{
    [RequireComponent(typeof(CanvasGroup))]
    public sealed class BattleLoadWaitingOverlay : MonoBehaviour
    {
        private const string TitleCopy = "전투 준비 중";
        private const string DescriptionCopy =
            "서버 전투 시작을 기다리고 있습니다.";

        [SerializeField] private GameObject panel;
        [SerializeField] private Text titleLabel;
        [SerializeField] private Text descriptionLabel;

        private CanvasGroup canvas;

        private void Awake()
        {
            EnsureCanvas();
            if (titleLabel != null)
            {
                titleLabel.text = TitleCopy;
            }
            if (descriptionLabel != null)
            {
                descriptionLabel.text = DescriptionCopy;
            }
        }

        public void Render(BattleLoadReadModel readModel)
        {
            Render(readModel != null && readModel.IsWaiting);
        }

        public void Render(bool visible)
        {
            EnsureCanvas();
            panel?.SetActive(true);
            canvas.alpha = visible ? 1 : 0;
            canvas.interactable = visible;
            canvas.blocksRaycasts = visible;
        }

        private void EnsureCanvas()
        {
            canvas ??= GetComponent<CanvasGroup>();
        }
    }
}
