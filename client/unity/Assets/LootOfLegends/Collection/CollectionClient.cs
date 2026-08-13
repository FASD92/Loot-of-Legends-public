using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using UnityEngine;

namespace LootOfLegends.Collection
{
    public sealed class CollectionItem
    {
        public CollectionItem(ulong itemId, ulong quantity, ulong value)
        {
            if (itemId == 0 || quantity == 0 || value == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(itemId));
            }
            ItemId = itemId;
            Quantity = quantity;
            Value = value;
        }

        public ulong ItemId { get; }
        public ulong Quantity { get; }
        public ulong Value { get; }
    }

    public sealed class CollectionSnapshot
    {
        private readonly IReadOnlyList<CollectionItem> items;

        public CollectionSnapshot(
            IReadOnlyList<CollectionItem> items,
            ulong wallet,
            long pendingSettlementCount)
        {
            if (items == null)
            {
                throw new ArgumentNullException(nameof(items));
            }
            if (pendingSettlementCount < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(pendingSettlementCount));
            }
            var copy = new List<CollectionItem>(items.Count);
            ulong previous = 0;
            foreach (CollectionItem item in items)
            {
                if (item == null || item.ItemId <= previous)
                {
                    throw new ArgumentException(
                        "Collection items must be unique and ordered by ItemId", nameof(items));
                }
                copy.Add(item);
                previous = item.ItemId;
            }
            this.items = new ReadOnlyCollection<CollectionItem>(copy);
            Wallet = wallet;
            PendingSettlementCount = pendingSettlementCount;
        }

        public IReadOnlyList<CollectionItem> Items => items;
        public ulong Wallet { get; }
        public long PendingSettlementCount { get; }
    }

    public sealed class CollectionProtocolException : Exception
    {
        public CollectionProtocolException(string message)
            : base(message)
        {
        }

        public CollectionProtocolException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    public static class CollectionSnapshotCodec
    {
        [Serializable]
        private sealed class ItemEnvelope
        {
            public string itemId;
            public string quantity;
            public string value;
        }

        [Serializable]
        private sealed class CollectionEnvelope
        {
            public ItemEnvelope[] items;
            public string wallet;
            public long pendingSettlementCount;
            public string freshness;
        }

        public static CollectionSnapshot Decode(string json)
        {
            if (string.IsNullOrWhiteSpace(json) ||
                !HasProperty(json, "items") ||
                !HasProperty(json, "wallet") ||
                !HasProperty(json, "pendingSettlementCount") ||
                !HasProperty(json, "freshness"))
            {
                throw new CollectionProtocolException("Collection response is incomplete");
            }
            CollectionEnvelope envelope;
            try
            {
                envelope = JsonUtility.FromJson<CollectionEnvelope>(json);
            }
            catch (ArgumentException error)
            {
                throw new CollectionProtocolException("Collection response is not valid JSON", error);
            }
            if (envelope == null || envelope.items == null || envelope.freshness != "Fresh" ||
                envelope.pendingSettlementCount < 0)
            {
                throw new CollectionProtocolException("Collection response metadata is invalid");
            }

            ulong wallet = ParseUnsigned(envelope.wallet, false, "wallet");
            var items = new List<CollectionItem>(envelope.items.Length);
            foreach (ItemEnvelope item in envelope.items)
            {
                if (item == null)
                {
                    throw new CollectionProtocolException("Collection item cannot be null");
                }
                try
                {
                    items.Add(new CollectionItem(
                        ParseUnsigned(item.itemId, true, "itemId"),
                        ParseUnsigned(item.quantity, true, "quantity"),
                        ParseUnsigned(item.value, true, "value")));
                }
                catch (ArgumentException error)
                {
                    throw new CollectionProtocolException("Collection item is invalid", error);
                }
            }
            try
            {
                return new CollectionSnapshot(items, wallet, envelope.pendingSettlementCount);
            }
            catch (ArgumentException error)
            {
                throw new CollectionProtocolException("Collection item order is invalid", error);
            }
        }

        private static bool HasProperty(string json, string name)
        {
            return json.IndexOf("\"" + name + "\"", StringComparison.Ordinal) >= 0;
        }

        private static ulong ParseUnsigned(string text, bool positive, string field)
        {
            if (string.IsNullOrEmpty(text) ||
                !ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out ulong value) ||
                value.ToString(CultureInfo.InvariantCulture) != text ||
                (positive && value == 0))
            {
                throw new CollectionProtocolException(field + " is not canonical uint64");
            }
            return value;
        }
    }

    public sealed class CollectionReadModel
    {
        private IReadOnlyList<CollectionItem> items =
            new ReadOnlyCollection<CollectionItem>(new List<CollectionItem>());

        public IReadOnlyList<CollectionItem> Items => items;
        public ulong Wallet { get; private set; }
        public long PendingSettlementCount { get; private set; }
        public CollectionPresentationState State { get; private set; } =
            CollectionPresentationState.Loading;
        public bool IsFresh => State == CollectionPresentationState.Fresh;
        public bool HasConfirmedSnapshot { get; private set; }

        public void BeginRefresh()
        {
            State = CollectionPresentationState.Loading;
        }

        public void Apply(CollectionSnapshot snapshot)
        {
            if (snapshot == null)
            {
                throw new ArgumentNullException(nameof(snapshot));
            }
            items = new ReadOnlyCollection<CollectionItem>(
                new List<CollectionItem>(snapshot.Items));
            Wallet = snapshot.Wallet;
            PendingSettlementCount = snapshot.PendingSettlementCount;
            HasConfirmedSnapshot = true;
            State = CollectionPresentationState.Fresh;
        }

        public void MarkUnavailable()
        {
            State = HasConfirmedSnapshot
                ? CollectionPresentationState.Stale
                : CollectionPresentationState.Error;
        }

        public CollectionPresentationSnapshot Snapshot()
        {
            var presentationItems = new List<CollectionPresentationItem>(items.Count);
            foreach (CollectionItem item in items)
            {
                presentationItems.Add(new CollectionPresentationItem(
                    item.ItemId,
                    item.Quantity,
                    item.Value));
            }
            return new CollectionPresentationSnapshot(
                State,
                presentationItems,
                Wallet,
                PendingSettlementCount);
        }
    }
}
