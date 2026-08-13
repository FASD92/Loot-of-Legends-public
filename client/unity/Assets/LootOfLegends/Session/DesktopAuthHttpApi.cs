using System;
using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using UnityEngine;

namespace LootOfLegends.Session
{
    public sealed class DesktopAuthHttpApi : IDesktopAuthApi
    {
        private readonly HttpClient http;
        private readonly Uri metaBaseUri;

        public DesktopAuthHttpApi(HttpClient http, Uri metaBaseUri)
        {
            this.http = http ?? throw new ArgumentNullException(nameof(http));
            this.metaBaseUri = metaBaseUri ??
                throw new ArgumentNullException(nameof(metaBaseUri));
            if (!metaBaseUri.IsAbsoluteUri ||
                (metaBaseUri.Scheme != Uri.UriSchemeHttps &&
                 !(metaBaseUri.Scheme == Uri.UriSchemeHttp &&
                   metaBaseUri.Host == "127.0.0.1")) ||
                !string.IsNullOrEmpty(metaBaseUri.UserInfo) ||
                !string.IsNullOrEmpty(metaBaseUri.Query) ||
                !string.IsNullOrEmpty(metaBaseUri.Fragment))
            {
                throw new ArgumentException("Invalid Meta base URI", nameof(metaBaseUri));
            }
        }

        public async Task<DesktopAuthStart> StartAsync(
            Uri loopbackRedirectUri,
            string state,
            string codeChallenge,
            CancellationToken cancellationToken)
        {
            var requestBody = new StartRequest
            {
                loopbackRedirectUri = loopbackRedirectUri.AbsoluteUri,
                state = state,
                codeChallenge = codeChallenge
            };
            string responseBody = await PostAsync(
                "/api/v1/desktop-auth/attempts",
                JsonUtility.ToJson(requestBody),
                cancellationToken).ConfigureAwait(false);
            RejectProviderTokens(responseBody);
            StartResponse response = Decode<StartResponse>(responseBody);
            if (response == null ||
                !Uri.TryCreate(response.authorizationUrl, UriKind.Absolute, out Uri authorization) ||
                !TryTimestamp(response.expiresAt, out DateTimeOffset expiresAt))
            {
                throw new DesktopAuthException("Desktop auth start response is invalid");
            }
            return new DesktopAuthStart(authorization, expiresAt);
        }

        public async Task<MetaSessionIssued> ExchangeAsync(
            string handoffCode,
            string state,
            string codeVerifier,
            CancellationToken cancellationToken)
        {
            var requestBody = new ExchangeRequest
            {
                handoffCode = handoffCode,
                state = state,
                codeVerifier = codeVerifier
            };
            string responseBody = await PostAsync(
                "/api/v1/desktop-auth/exchanges",
                JsonUtility.ToJson(requestBody),
                cancellationToken).ConfigureAwait(false);
            RejectProviderTokens(responseBody);
            MetaSessionResponse response = Decode<MetaSessionResponse>(responseBody);
            if (response == null ||
                !TryTimestamp(response.expiresAt, out DateTimeOffset expiresAt))
            {
                throw new DesktopAuthException("Meta session response is invalid");
            }
            return new MetaSessionIssued(response.metaSession, expiresAt);
        }

        private async Task<string> PostAsync(
            string path,
            string json,
            CancellationToken cancellationToken)
        {
            using (var request = new HttpRequestMessage(HttpMethod.Post, new Uri(metaBaseUri, path)))
            {
                request.Content = new StringContent(json, Encoding.UTF8, "application/json");
                using (HttpResponseMessage response =
                    await http.SendAsync(request, cancellationToken).ConfigureAwait(false))
                {
                    if (!response.IsSuccessStatusCode)
                    {
                        throw new DesktopAuthException(
                            "Meta desktop auth request failed with status " +
                            (int)response.StatusCode);
                    }
                    return await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                }
            }
        }

        private static T Decode<T>(string json) where T : class
        {
            try
            {
                return JsonUtility.FromJson<T>(json);
            }
            catch (ArgumentException error)
            {
                throw new DesktopAuthException("Meta desktop auth response is not JSON", error);
            }
        }

        private static bool TryTimestamp(string text, out DateTimeOffset timestamp)
        {
            return DateTimeOffset.TryParse(
                text,
                CultureInfo.InvariantCulture,
                DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
                out timestamp);
        }

        private static void RejectProviderTokens(string json)
        {
            if (json.IndexOf("\"accessToken\"", StringComparison.Ordinal) >= 0 ||
                json.IndexOf("\"refreshToken\"", StringComparison.Ordinal) >= 0 ||
                json.IndexOf("\"idToken\"", StringComparison.Ordinal) >= 0)
            {
                throw new DesktopAuthException(
                    "Meta response crossed the provider-token boundary");
            }
        }

        [Serializable]
        private sealed class StartRequest
        {
            public string loopbackRedirectUri;
            public string state;
            public string codeChallenge;
        }

        [Serializable]
        private sealed class StartResponse
        {
            public string authorizationUrl;
            public string expiresAt;
        }

        [Serializable]
        private sealed class ExchangeRequest
        {
            public string handoffCode;
            public string state;
            public string codeVerifier;
        }

        [Serializable]
        private sealed class MetaSessionResponse
        {
            public string metaSession;
            public string expiresAt;
        }
    }
}
