using LootOfLegends.Battle.Loot;
using UnityEngine;

namespace LootOfLegends.Presentation
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class BattleLootMarkerPresenter : MonoBehaviour
    {
        private TextMesh label;

        private void Awake()
        {
            label = GetComponent<TextMesh>();
        }

        public void Render(BattleLootDropView drop)
        {
            if (drop == null)
            {
                label.text = string.Empty;
                return;
            }
            transform.position = new Vector3(
                drop.PositionXMillimeters / 1000f,
                0f,
                drop.PositionYMillimeters / 1000f);
            label.text = $"Item {drop.ItemId} x{drop.Quantity} {drop.StateName}";
        }
    }
}
