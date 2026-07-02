using System;
using System.IO;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public static class Release0ClientConfig
    {
        public const string FileName = "Release0ClientConfig.json";
        public const string DefaultMetaBaseUrl = "http://127.0.0.1:8080";

        public static string LoadMetaBaseUrl(
            string streamingAssetsPath,
            string fallbackMetaBaseUrl)
        {
            string normalizedFallback = NormalizeFallbackMetaBaseUrl(fallbackMetaBaseUrl);
            if (string.IsNullOrWhiteSpace(streamingAssetsPath))
            {
                return normalizedFallback;
            }

            string configPath = Path.Combine(streamingAssetsPath, FileName);
            if (!File.Exists(configPath))
            {
                return normalizedFallback;
            }

            try
            {
                Release0ClientConfigJson config =
                    JsonUtility.FromJson<Release0ClientConfigJson>(
                        File.ReadAllText(configPath));
                string configuredMetaBaseUrl =
                    NormalizeConfiguredMetaBaseUrl(config?.metaBaseUrl);
                return string.IsNullOrEmpty(configuredMetaBaseUrl) ?
                    normalizedFallback :
                    configuredMetaBaseUrl;
            }
            catch (Exception)
            {
                return normalizedFallback;
            }
        }

        private static string NormalizeFallbackMetaBaseUrl(string metaBaseUrl)
        {
            return string.IsNullOrWhiteSpace(metaBaseUrl) ?
                DefaultMetaBaseUrl :
                metaBaseUrl.Trim().TrimEnd('/');
        }

        private static string NormalizeConfiguredMetaBaseUrl(string metaBaseUrl)
        {
            return string.IsNullOrWhiteSpace(metaBaseUrl) ?
                string.Empty :
                metaBaseUrl.Trim().TrimEnd('/');
        }

        [Serializable]
        private sealed class Release0ClientConfigJson
        {
            public string metaBaseUrl;
        }
    }
}
