using LootOfLegends.Battle.Combat;
using UnityEngine;

namespace LootOfLegends.Presentation
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class BattleCombatPresenter : MonoBehaviour
    {
        private TextMesh label;

        private void Awake()
        {
            label = GetComponent<TextMesh>();
        }

        public void Render(BattleCombatReadModel readModel)
        {
            if (readModel == null)
            {
                label.text = string.Empty;
                return;
            }
            string outcome = string.IsNullOrEmpty(readModel.OutcomeName)
                ? string.Empty
                : " " + readModel.OutcomeName;
            label.text = readModel.HasMonster
                ? $"HP {readModel.HitPoints}/{readModel.MaximumHitPoints}{outcome}"
                : outcome.TrimStart();
        }
    }
}
