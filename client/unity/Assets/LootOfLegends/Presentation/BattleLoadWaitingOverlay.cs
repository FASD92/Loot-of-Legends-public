using LootOfLegends.Battle;
using UnityEngine;

namespace LootOfLegends.Presentation
{
    [RequireComponent(typeof(CanvasGroup))]
    public sealed class BattleLoadWaitingOverlay : MonoBehaviour
    {
        private CanvasGroup canvas;

        private void Awake()
        {
            canvas = GetComponent<CanvasGroup>();
        }

        public void Render(BattleLoadReadModel readModel)
        {
            bool visible = readModel != null && readModel.IsWaiting;
            canvas.alpha = visible ? 1 : 0;
            canvas.interactable = visible;
            canvas.blocksRaycasts = visible;
        }
    }
}
