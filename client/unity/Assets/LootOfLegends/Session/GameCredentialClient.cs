using System;
using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;
using UnityEngine;

namespace LootOfLegends.Session
{
    public sealed class IssuedGameCredential
    {
        public IssuedGameCredential(string credential, DateTimeOffset expiresAt)
        {
            if (!DesktopAuthValidation.IsOpaqueCode(credential))
            {
                throw new ArgumentException("Invalid game credential", nameof(credential));
            }
            Credential = credential;
            ExpiresAt = expiresAt;
        }

        public string Credential { get; }
        public DateTimeOffset ExpiresAt { get; }
    }

    public sealed class GameCredentialHttpApi
    {
        private const int MaximumResponseBytes = 4096;
        private readonly HttpClient http;
        private readonly Uri issueUri;
        private readonly Action<HttpRequestMessage> authorize;

        public GameCredentialHttpApi(
            HttpClient http,
            Uri metaBaseUri,
            Action<HttpRequestMessage> authorize)
        {
            this.http = http ?? throw new ArgumentNullException(nameof(http));
            if (metaBaseUri == null || !metaBaseUri.IsAbsoluteUri ||
                (metaBaseUri.Scheme != Uri.UriSchemeHttps &&
                 !(metaBaseUri.Scheme == Uri.UriSchemeHttp &&
                   metaBaseUri.Host == "127.0.0.1")) ||
                !string.IsNullOrEmpty(metaBaseUri.UserInfo) ||
                !string.IsNullOrEmpty(metaBaseUri.Query) ||
                !string.IsNullOrEmpty(metaBaseUri.Fragment))
            {
                throw new ArgumentException("Invalid Meta base URI", nameof(metaBaseUri));
            }
            this.authorize = authorize ?? throw new ArgumentNullException(nameof(authorize));
            issueUri = new Uri(metaBaseUri, "/api/v1/game-credentials");
        }

        public async Task<IssuedGameCredential> IssueAsync(
            CancellationToken cancellationToken)
        {
            using (var request = new HttpRequestMessage(HttpMethod.Post, issueUri))
            {
                authorize(request);
                using (HttpResponseMessage response =
                    await http.SendAsync(request, cancellationToken).ConfigureAwait(false))
                {
                    if (!response.IsSuccessStatusCode ||
                        response.Content.Headers.ContentLength > MaximumResponseBytes)
                    {
                        throw new DesktopAuthException(
                            "Game credential request was rejected");
                    }
                    string body = await response.Content.ReadAsStringAsync()
                        .ConfigureAwait(false);
                    if (Encoding.UTF8.GetByteCount(body) > MaximumResponseBytes ||
                        body.IndexOf("\"accessToken\"", StringComparison.Ordinal) >= 0 ||
                        body.IndexOf("\"refreshToken\"", StringComparison.Ordinal) >= 0 ||
                        body.IndexOf("\"idToken\"", StringComparison.Ordinal) >= 0)
                    {
                        throw new DesktopAuthException(
                            "Game credential response crossed its bounded contract");
                    }
                    IssueResponse decoded;
                    try
                    {
                        decoded = JsonUtility.FromJson<IssueResponse>(body);
                    }
                    catch (ArgumentException error)
                    {
                        throw new DesktopAuthException(
                            "Game credential response is not JSON", error);
                    }
                    if (decoded == null ||
                        !DateTimeOffset.TryParse(
                            decoded.expiresAt,
                            CultureInfo.InvariantCulture,
                            DateTimeStyles.AssumeUniversal |
                                DateTimeStyles.AdjustToUniversal,
                            out DateTimeOffset expiresAt) ||
                        expiresAt <= DateTimeOffset.UtcNow)
                    {
                        throw new DesktopAuthException(
                            "Game credential response is invalid");
                    }
                    try
                    {
                        return new IssuedGameCredential(decoded.credential, expiresAt);
                    }
                    catch (ArgumentException error)
                    {
                        throw new DesktopAuthException(
                            "Game credential response is invalid", error);
                    }
                }
            }
        }

        [Serializable]
        private sealed class IssueResponse
        {
            public string credential;
            public string expiresAt;
        }
    }

    public sealed class GameSessionAuthenticator
    {
        private readonly ITcpCommandSender sender;
        private readonly PlayerSessionReadModel session;
        private long nextRequestId;

        public GameSessionAuthenticator(
            ITcpCommandSender sender,
            PlayerSessionReadModel session)
        {
            this.sender = sender ?? throw new ArgumentNullException(nameof(sender));
            this.session = session ?? throw new ArgumentNullException(nameof(session));
        }

        public async Task AuthenticateAsync(
            string credential,
            CancellationToken cancellationToken)
        {
            long request = Interlocked.Increment(ref nextRequestId);
            if (request <= 0)
            {
                throw new InvalidOperationException("Authentication request id exhausted");
            }
            var changed = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            Action observe = () =>
            {
                if (session.State == PlayerSessionState.Authenticated ||
                    session.State == PlayerSessionState.Disconnected)
                {
                    changed.TrySetResult(true);
                }
            };

            session.Changed += observe;
            using (cancellationToken.Register(() => changed.TrySetCanceled()))
            {
                try
                {
                    session.BeginAuthentication();
                    await sender.SendAsync(
                        SessionProtocolCodec.EncodeAuthenticateGameSession(
                            (ulong)request, credential),
                        cancellationToken).ConfigureAwait(false);
                    observe();
                    await changed.Task.ConfigureAwait(false);
                }
                finally
                {
                    session.Changed -= observe;
                }
            }
            if (session.State != PlayerSessionState.Authenticated)
            {
                throw new DesktopAuthException("Game authentication was rejected");
            }
        }
    }
}
