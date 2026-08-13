using System;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.Session
{
    public interface IDesktopAuthApi
    {
        Task<DesktopAuthStart> StartAsync(
            Uri loopbackRedirectUri,
            string state,
            string codeChallenge,
            CancellationToken cancellationToken);

        Task<MetaSessionIssued> ExchangeAsync(
            string handoffCode,
            string state,
            string codeVerifier,
            CancellationToken cancellationToken);
    }

    public interface ILoopbackAuthListenerFactory
    {
        ILoopbackAuthListener Listen();
    }

    public interface ILoopbackAuthListener : IDisposable
    {
        Uri CallbackUri { get; }
        Task<LoopbackAuthCallback> WaitAsync(CancellationToken cancellationToken);
    }

    public interface ISystemBrowser
    {
        void Open(Uri uri);
    }

    public sealed class DesktopAuthStart
    {
        public DesktopAuthStart(Uri authorizationUrl, DateTimeOffset expiresAt)
        {
            AuthorizationUrl = authorizationUrl ??
                throw new ArgumentNullException(nameof(authorizationUrl));
            if (!AuthorizationUrl.IsAbsoluteUri || AuthorizationUrl.Scheme != Uri.UriSchemeHttps)
            {
                throw new ArgumentException(
                    "Authorization URL must be absolute HTTPS", nameof(authorizationUrl));
            }
            ExpiresAt = expiresAt;
        }

        public Uri AuthorizationUrl { get; }
        public DateTimeOffset ExpiresAt { get; }
    }

    public sealed class LoopbackAuthCallback
    {
        public LoopbackAuthCallback(string handoffCode, string state)
        {
            if (!DesktopAuthValidation.IsOpaqueCode(handoffCode))
            {
                throw new ArgumentException("Invalid handoff code", nameof(handoffCode));
            }
            if (!DesktopAuthValidation.IsClientSecret(state))
            {
                throw new ArgumentException("Invalid callback state", nameof(state));
            }
            HandoffCode = handoffCode;
            State = state;
        }

        public string HandoffCode { get; }
        public string State { get; }
    }

    public sealed class MetaSessionIssued
    {
        public MetaSessionIssued(string metaSession, DateTimeOffset expiresAt)
        {
            if (!DesktopAuthValidation.IsOpaqueCode(metaSession))
            {
                throw new ArgumentException("Invalid Meta session", nameof(metaSession));
            }
            MetaSession = metaSession;
            ExpiresAt = expiresAt;
        }

        public string MetaSession { get; }
        public DateTimeOffset ExpiresAt { get; }
    }

    public sealed class DesktopAuthException : Exception
    {
        public DesktopAuthException(string message)
            : base(message)
        {
        }

        public DesktopAuthException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    public sealed class MetaSessionState
    {
        private readonly object gate = new object();
        private string metaSession;
        private DateTimeOffset expiresAt;

        public bool IsAuthenticated
        {
            get
            {
                lock (gate)
                {
                    return metaSession != null && DateTimeOffset.UtcNow < expiresAt;
                }
            }
        }

        public DateTimeOffset ExpiresAt
        {
            get
            {
                lock (gate)
                {
                    return expiresAt;
                }
            }
        }

        public void Accept(MetaSessionIssued issued)
        {
            if (issued == null)
            {
                throw new ArgumentNullException(nameof(issued));
            }
            if (issued.ExpiresAt <= DateTimeOffset.UtcNow)
            {
                throw new DesktopAuthException("Meta session is already expired");
            }
            lock (gate)
            {
                metaSession = issued.MetaSession;
                expiresAt = issued.ExpiresAt;
            }
        }

        public void Authorize(HttpRequestMessage request)
        {
            if (request == null)
            {
                throw new ArgumentNullException(nameof(request));
            }
            lock (gate)
            {
                if (metaSession == null || DateTimeOffset.UtcNow >= expiresAt)
                {
                    metaSession = null;
                    expiresAt = default;
                    throw new DesktopAuthException("Meta session is unavailable");
                }
                request.Headers.Authorization =
                    new AuthenticationHeaderValue("Bearer", metaSession);
            }
        }

        public void Clear()
        {
            lock (gate)
            {
                metaSession = null;
                expiresAt = default;
            }
        }
    }

    internal static class DesktopAuthValidation
    {
        public static bool IsOpaqueCode(string value)
        {
            return value != null && value.Length == 43 && IsBase64Url(value);
        }

        public static bool IsClientSecret(string value)
        {
            if (value == null || value.Length < 43 || value.Length > 128)
            {
                return false;
            }
            foreach (char character in value)
            {
                if (!(character >= 'A' && character <= 'Z') &&
                    !(character >= 'a' && character <= 'z') &&
                    !(character >= '0' && character <= '9') &&
                    character != '-' && character != '.' &&
                    character != '_' && character != '~')
                {
                    return false;
                }
            }
            return true;
        }

        private static bool IsBase64Url(string value)
        {
            foreach (char character in value)
            {
                if (!(character >= 'A' && character <= 'Z') &&
                    !(character >= 'a' && character <= 'z') &&
                    !(character >= '0' && character <= '9') &&
                    character != '-' && character != '_')
                {
                    return false;
                }
            }
            return true;
        }
    }
}
