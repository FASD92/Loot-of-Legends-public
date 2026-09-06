using System;
using LootOfLegends.Collection;
using LootOfLegends.Presentation.Common;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Collection
{
    public sealed class CollectionItemRowView : MonoBehaviour
    {
        private const string CommonRelicKey = "loot.relic.common";
        private const string RareRelicKey = "loot.relic.rare";

        [SerializeField] private PresentationCatalog catalog;
        [SerializeField] private Image itemIcon;
        [SerializeField] private Text itemIdLabel;
        [SerializeField] private Text quantityLabel;
        [SerializeField] private Text valueLabel;

        public void Render(CollectionPresentationItem item)
        {
            if (item == null)
            {
                throw new ArgumentNullException(nameof(item));
            }

            RenderItem(item.ItemId);
            quantityLabel.text = item.Quantity.ToString();
            valueLabel.text = item.Value.ToString();
        }

        private void RenderItem(ulong itemId)
        {
            itemIdLabel.text = itemId.ToString();
            string key = itemId == 1 ? CommonRelicKey :
                itemId == 2 ? RareRelicKey : null;
            bool knownRelic = key != null;
            itemIcon.gameObject.SetActive(knownRelic);
            itemIdLabel.gameObject.SetActive(!knownRelic);

            if (knownRelic)
            {
                itemIcon.sprite = catalog.Resolve<Sprite>(key);
            }
            else
            {
                itemIcon.sprite = null;
            }
        }
    }
}
