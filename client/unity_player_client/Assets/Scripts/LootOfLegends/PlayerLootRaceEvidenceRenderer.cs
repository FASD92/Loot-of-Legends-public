using System.Text;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerLootRaceEvidenceRenderer : MonoBehaviour
    {
        public const int DefaultFontSize = 28;
        public const float DefaultCharacterSize = 0.16f;

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

        public bool RenderEvidence(
            PlayerLootResolved resolved,
            PlayerLootRejected rejected,
            PlayerInventorySnapshot inventorySnapshot)
        {
            bool passed = IsFullPass(resolved, rejected, inventorySnapshot);
            EnsureTextMesh().text = passed ?
                BuildPassText(resolved) :
                BuildPendingText(resolved, rejected, inventorySnapshot);
            return passed;
        }

        public bool RenderCapturedEvidence()
        {
            if (networkSessionController == null ||
                !networkSessionController.LootResolvedCaptured ||
                !networkSessionController.LootRejectedCaptured ||
                !networkSessionController.InventorySnapshotCaptured)
            {
                Clear();
                return false;
            }

            return RenderEvidence(
                networkSessionController.LootResolved,
                networkSessionController.LootRejected,
                networkSessionController.InventorySnapshot);
        }

        public void Clear()
        {
            EnsureTextMesh().text = string.Empty;
        }

        private static bool IsFullPass(
            PlayerLootResolved resolved,
            PlayerLootRejected rejected,
            PlayerInventorySnapshot inventorySnapshot)
        {
            return IsDisplayableResolved(resolved) &&
                IsMatchingAlreadyClaimedRejection(resolved, rejected) &&
                InventoryConfirmsWinnerItem(resolved, inventorySnapshot);
        }

        private static bool IsDisplayableResolved(PlayerLootResolved resolved)
        {
            return resolved.RoomId != 0U &&
                resolved.DropId != 0U &&
                resolved.WinnerSessionId != 0UL &&
                resolved.ItemId != 0U &&
                resolved.Quantity != 0;
        }

        private static bool IsMatchingAlreadyClaimedRejection(
            PlayerLootResolved resolved,
            PlayerLootRejected rejected)
        {
            return resolved.RoomId != 0U &&
                rejected.RoomId == resolved.RoomId &&
                rejected.DropId == resolved.DropId &&
                rejected.Reason == PlayerLootRejectReason.AlreadyClaimed;
        }

        private static bool InventoryConfirmsWinnerItem(
            PlayerLootResolved resolved,
            PlayerInventorySnapshot inventorySnapshot)
        {
            if (inventorySnapshot.SessionId != resolved.WinnerSessionId ||
                inventorySnapshot.MaxWeight == 0 ||
                inventorySnapshot.CurrentWeight > inventorySnapshot.MaxWeight)
            {
                return false;
            }

            for (int index = 0; index < inventorySnapshot.Count; ++index)
            {
                PlayerInventoryEntry entry = inventorySnapshot.EntryAt(index);
                if (entry.ItemId == resolved.ItemId &&
                    entry.Quantity >= resolved.Quantity)
                {
                    return true;
                }
            }

            return false;
        }

        private static string BuildPassText(PlayerLootResolved resolved)
        {
            return new StringBuilder()
                .Append("Loot Race PASS")
                .Append('\n')
                .Append("Drop ")
                .Append(resolved.DropId)
                .Append('\n')
                .Append("Winner ")
                .Append(resolved.WinnerSessionId)
                .Append('\n')
                .Append("Loser: AlreadyClaimed")
                .Append('\n')
                .Append("Inventory: Item ")
                .Append(resolved.ItemId)
                .Append(" x")
                .Append(resolved.Quantity)
                .ToString();
        }

        private static string BuildPendingText(
            PlayerLootResolved resolved,
            PlayerLootRejected rejected,
            PlayerInventorySnapshot inventorySnapshot)
        {
            StringBuilder builder = new StringBuilder();
            builder.Append("Loot Race Pending");

            if (IsDisplayableResolved(resolved))
            {
                builder
                    .Append('\n')
                    .Append("Resolved: Drop ")
                    .Append(resolved.DropId)
                    .Append(" Winner ")
                    .Append(resolved.WinnerSessionId);
            }
            else
            {
                builder.Append('\n').Append("Resolved: Missing");
            }

            builder
                .Append('\n')
                .Append(IsMatchingAlreadyClaimedRejection(resolved, rejected) ?
                    "Rejected: AlreadyClaimed" :
                    "Rejected: Missing AlreadyClaimed for same drop");

            builder
                .Append('\n')
                .Append(InventoryConfirmsWinnerItem(resolved, inventorySnapshot) ?
                    "Inventory: Winner item confirmed" :
                    "Inventory: Missing winner item");

            return builder.ToString();
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
            textMesh.color = Color.green;
            return textMesh;
        }
    }
}
