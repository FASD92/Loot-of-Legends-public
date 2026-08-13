using System.Text;
using LootOfLegends.Collection;
using UnityEngine;

namespace LootOfLegends.Presentation.Collection
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class CollectionTextView : MonoBehaviour, ICollectionView
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

        public void Render(CollectionPresentationSnapshot snapshot)
        {
            var copy = new StringBuilder();
            copy.AppendLine("COLLECTION");
            copy.Append("Wallet  ").Append(snapshot.Wallet).AppendLine();
            copy.Append("Pending  ").Append(snapshot.PendingSettlementCount).AppendLine();
            foreach (CollectionPresentationItem item in snapshot.Items)
            {
                copy.Append("Item ").Append(item.ItemId)
                    .Append("  x").Append(item.Quantity)
                    .Append("  value ").Append(item.Value).AppendLine();
            }
            copy.Append(snapshot.StatusCopy);
            label.text = copy.ToString();
        }
    }
}
