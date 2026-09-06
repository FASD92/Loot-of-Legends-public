using System;
using System.Collections.Generic;
using UnityEngine;
using Object = UnityEngine.Object;

namespace LootOfLegends.Presentation.Common
{
    [CreateAssetMenu(
        fileName = "PresentationCatalog",
        menuName = "Loot of Legends/Presentation Catalog")]
    public sealed class PresentationCatalog : ScriptableObject
    {
        [Serializable]
        private sealed class Entry
        {
            [SerializeField] private string key;
            [SerializeField] private Object asset;

            public string Key => key;
            public Object Asset => asset;
        }

        [SerializeField] private Entry[] entries = Array.Empty<Entry>();

        public T Resolve<T>(string key) where T : Object
        {
            if (string.IsNullOrWhiteSpace(key))
            {
                throw new ArgumentException("Presentation key is required", nameof(key));
            }

            var assets = new Dictionary<string, Object>(StringComparer.Ordinal);
            foreach (Entry entry in entries)
            {
                if (entry == null || string.IsNullOrWhiteSpace(entry.Key) ||
                    entry.Asset == null)
                {
                    throw new InvalidOperationException(
                        "Presentation catalog contains an incomplete entry");
                }
                if (assets.ContainsKey(entry.Key))
                {
                    throw new InvalidOperationException(
                        "Presentation catalog contains a duplicate key: " + entry.Key);
                }
                assets.Add(entry.Key, entry.Asset);
            }

            if (!assets.TryGetValue(key, out Object asset))
            {
                throw new KeyNotFoundException("Missing presentation key: " + key);
            }
            if (!(asset is T typed))
            {
                throw new InvalidOperationException(
                    $"Presentation asset {key} is not {typeof(T).Name}");
            }
            return typed;
        }
    }
}
