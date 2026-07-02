using System.Text;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerInventoryStatusRenderer : MonoBehaviour
    {
        public const int DefaultFontSize = 32;
        public const float DefaultCharacterSize = 0.18f;

        [SerializeField]
        private PlayerNetworkSessionController networkSessionController;

        private TextMesh textMesh;

        public PlayerNetworkSessionController NetworkSessionController
        {
            get => networkSessionController;
            set => networkSessionController = value;
        }

        public TextMesh StatusText => EnsureTextMesh();

        public string Text => EnsureTextMesh().text;

        public void RenderInventorySnapshot(PlayerInventorySnapshot snapshot)
        {
            if (!IsDisplayable(snapshot))
            {
                Clear();
                return;
            }

            StringBuilder builder = new StringBuilder();
            builder
                .Append("Inventory ")
                .Append(snapshot.CurrentWeight)
                .Append('/')
                .Append(snapshot.MaxWeight);

            if (snapshot.Count == 0)
            {
                builder.Append('\n').Append("Empty");
            }
            else
            {
                for (int index = 0; index < snapshot.Count; ++index)
                {
                    PlayerInventoryEntry entry = snapshot.EntryAt(index);
                    builder
                        .Append('\n')
                        .Append("Item ")
                        .Append(entry.ItemId)
                        .Append(" x")
                        .Append(entry.Quantity);
                }
            }

            EnsureTextMesh().text = builder.ToString();
        }

        public bool RenderCapturedInventorySnapshot()
        {
            if (networkSessionController == null ||
                !networkSessionController.InventorySnapshotCaptured)
            {
                return false;
            }

            RenderInventorySnapshot(networkSessionController.InventorySnapshot);
            return !string.IsNullOrEmpty(Text);
        }

        public void Clear()
        {
            EnsureTextMesh().text = string.Empty;
        }

        private static bool IsDisplayable(PlayerInventorySnapshot snapshot)
        {
            return snapshot.SessionId != 0UL &&
                snapshot.MaxWeight != 0 &&
                snapshot.CurrentWeight <= snapshot.MaxWeight;
        }

        private TextMesh EnsureTextMesh()
        {
            if (textMesh != null)
            {
                return textMesh;
            }

            textMesh = GetComponent<TextMesh>();
            if (textMesh == null)
            {
                textMesh = gameObject.AddComponent<TextMesh>();
            }

            textMesh.fontSize = DefaultFontSize;
            textMesh.characterSize = DefaultCharacterSize;
            textMesh.anchor = TextAnchor.UpperLeft;
            textMesh.alignment = TextAlignment.Left;
            textMesh.color = Color.white;
            return textMesh;
        }
    }
}
