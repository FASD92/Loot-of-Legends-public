using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace LootOfLegends.Collection
{
    public enum CollectionPresentationState
    {
        Loading,
        Fresh,
        Stale,
        Error
    }

    public sealed class CollectionPresentationItem
    {
        public CollectionPresentationItem(ulong itemId, ulong quantity, ulong value)
        {
            ItemId = itemId;
            Quantity = quantity;
            Value = value;
        }

        public ulong ItemId { get; }
        public ulong Quantity { get; }
        public ulong Value { get; }
    }

    public sealed class CollectionPresentationSnapshot
    {
        private readonly IReadOnlyList<CollectionPresentationItem> items;

        public CollectionPresentationSnapshot(
            CollectionPresentationState state,
            IReadOnlyList<CollectionPresentationItem> items,
            ulong wallet,
            long pendingSettlementCount)
        {
            State = state;
            this.items = new ReadOnlyCollection<CollectionPresentationItem>(
                new List<CollectionPresentationItem>(items ??
                    throw new ArgumentNullException(nameof(items))));
            Wallet = wallet;
            PendingSettlementCount = pendingSettlementCount;
            StatusCopy = CopyFor(state);
        }

        public CollectionPresentationState State { get; }
        public IReadOnlyList<CollectionPresentationItem> Items => items;
        public ulong Wallet { get; }
        public long PendingSettlementCount { get; }
        public string StatusCopy { get; }

        private static string CopyFor(CollectionPresentationState state)
        {
            switch (state)
            {
                case CollectionPresentationState.Loading:
                    return "Collection을 불러오는 중입니다.";
                case CollectionPresentationState.Stale:
                    return "최근 확인된 Collection입니다. 다시 시도해 주세요.";
                case CollectionPresentationState.Error:
                    return "Collection을 불러오지 못했습니다. 다시 시도해 주세요.";
                default:
                    return string.Empty;
            }
        }
    }
}
