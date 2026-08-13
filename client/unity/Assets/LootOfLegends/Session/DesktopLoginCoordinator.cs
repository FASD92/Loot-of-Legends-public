using System;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.Session
{
    public sealed class DesktopLoginCoordinator
    {
        private const int TokenBytes = 32;
        private readonly IDesktopAuthApi api;
        private readonly ILoopbackAuthListenerFactory listenerFactory;
        private readonly ISystemBrowser browser;
        private readonly MetaSessionState session;
        private readonly SemaphoreSlim signInGate = new SemaphoreSlim(1, 1);

        public DesktopLoginCoordinator(
            IDesktopAuthApi api,
            ILoopbackAuthListenerFactory listenerFactory,
            ISystemBrowser browser,
            MetaSessionState session)
        {
            this.api = api ?? throw new ArgumentNullException(nameof(api));
            this.listenerFactory = listenerFactory ??
                throw new ArgumentNullException(nameof(listenerFactory));
            this.browser = browser ?? throw new ArgumentNullException(nameof(browser));
            this.session = session ?? throw new ArgumentNullException(nameof(session));
        }

        public async Task SignInAsync(CancellationToken cancellationToken)
        {
            await signInGate.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                string state = RandomToken();
                string verifier = RandomToken();
                using (ILoopbackAuthListener listener = listenerFactory.Listen())
                {
                    DesktopAuthStart started = await api.StartAsync(
                        listener.CallbackUri,
                        state,
                        S256(verifier),
                        cancellationToken).ConfigureAwait(false);
                    if (started.ExpiresAt <= DateTimeOffset.UtcNow)
                    {
                        throw new DesktopAuthException("Desktop auth attempt is already expired");
                    }

                    browser.Open(started.AuthorizationUrl);
                    LoopbackAuthCallback callback =
                        await listener.WaitAsync(cancellationToken).ConfigureAwait(false);
                    if (!SecureEquals(callback.State, state))
                    {
                        throw new DesktopAuthException("Desktop auth state mismatch");
                    }

                    MetaSessionIssued issued = await api.ExchangeAsync(
                        callback.HandoffCode,
                        state,
                        verifier,
                        cancellationToken).ConfigureAwait(false);
                    session.Accept(issued);
                }
            }
            finally
            {
                signInGate.Release();
            }
        }

        public static string S256(string verifier)
        {
            if (!DesktopAuthValidation.IsClientSecret(verifier))
            {
                throw new ArgumentException("Invalid PKCE verifier", nameof(verifier));
            }
            using (SHA256 sha256 = SHA256.Create())
            {
                return Base64Url(sha256.ComputeHash(Encoding.ASCII.GetBytes(verifier)));
            }
        }

        private static string RandomToken()
        {
            var bytes = new byte[TokenBytes];
            using (RandomNumberGenerator random = RandomNumberGenerator.Create())
            {
                random.GetBytes(bytes);
            }
            return Base64Url(bytes);
        }

        private static string Base64Url(byte[] bytes)
        {
            return Convert.ToBase64String(bytes)
                .TrimEnd('=')
                .Replace('+', '-')
                .Replace('/', '_');
        }

        private static bool SecureEquals(string left, string right)
        {
            byte[] leftBytes = Encoding.UTF8.GetBytes(left ?? string.Empty);
            byte[] rightBytes = Encoding.UTF8.GetBytes(right ?? string.Empty);
            int difference = leftBytes.Length ^ rightBytes.Length;
            int length = Math.Max(leftBytes.Length, rightBytes.Length);
            for (int index = 0; index < length; index++)
            {
                byte leftByte = index < leftBytes.Length ? leftBytes[index] : (byte)0;
                byte rightByte = index < rightBytes.Length ? rightBytes[index] : (byte)0;
                difference |= leftByte ^ rightByte;
            }
            return difference == 0;
        }
    }
}
