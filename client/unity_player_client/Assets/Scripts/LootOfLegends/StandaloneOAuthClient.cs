using System;
using System.Security.Cryptography;

namespace LootOfLegends.PlayerClient
{
    public static class StandaloneOAuthClient
    {
        public const string StartPath = "/api/release0/auth/standalone/start";
        public const int MaxStateLength = 128;

        public static string BuildStartUrl(string metaBaseUrl, string callbackUri, string state)
        {
            if (string.IsNullOrWhiteSpace(metaBaseUrl))
            {
                throw new ArgumentException("Meta base URL is required", nameof(metaBaseUrl));
            }

            if (string.IsNullOrWhiteSpace(callbackUri))
            {
                throw new ArgumentException("Callback URI is required", nameof(callbackUri));
            }

            if (!IsAllowedState(state))
            {
                throw new ArgumentException("State must be URL-safe and 128 characters or shorter", nameof(state));
            }

            return metaBaseUrl.TrimEnd('/') +
                StartPath +
                "?callback=" +
                Uri.EscapeDataString(callbackUri) +
                "&state=" +
                Uri.EscapeDataString(state);
        }

        public static string CreateState()
        {
            byte[] bytes = new byte[32];
            using (RandomNumberGenerator generator = RandomNumberGenerator.Create())
            {
                generator.GetBytes(bytes);
            }

            return Convert.ToBase64String(bytes)
                .TrimEnd('=')
                .Replace('+', '-')
                .Replace('/', '_');
        }

        public static bool IsAllowedState(string state)
        {
            if (string.IsNullOrEmpty(state) || state.Length > MaxStateLength)
            {
                return false;
            }

            foreach (char value in state)
            {
                if (!IsAllowedStateCharacter(value))
                {
                    return false;
                }
            }

            return true;
        }

        private static bool IsAllowedStateCharacter(char value)
        {
            return (value >= 'A' && value <= 'Z') ||
                (value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') ||
                value == '.' ||
                value == '_' ||
                value == '~' ||
                value == '-';
        }
    }
}
