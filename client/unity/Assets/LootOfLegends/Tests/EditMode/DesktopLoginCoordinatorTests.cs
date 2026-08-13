using System;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Session;
using NUnit.Framework;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class DesktopLoginCoordinatorTests
    {
        [Test]
        public async Task SystemBrowserHandoffExchangesPkceForMetaSessionOnly()
        {
            var api = new RecordingAuthApi();
            var listenerFactory = new FakeListenerFactory(api, false);
            var browser = new RecordingBrowser();
            var session = new MetaSessionState();
            var coordinator = new DesktopLoginCoordinator(
                api,
                listenerFactory,
                browser,
                session);

            await coordinator.SignInAsync(CancellationToken.None);

            Assert.That(api.StartState, Has.Length.EqualTo(43));
            Assert.That(api.StartChallenge, Has.Length.EqualTo(43));
            Assert.That(api.ExchangeState, Is.EqualTo(api.StartState));
            Assert.That(
                DesktopLoginCoordinator.S256(api.ExchangeVerifier),
                Is.EqualTo(api.StartChallenge));
            Assert.That(browser.Opened, Is.EqualTo(api.AuthorizationUrl));
            Assert.That(listenerFactory.Listener.Disposed, Is.True);
            Assert.That(session.IsAuthenticated, Is.True);

            using (var request = new HttpRequestMessage())
            {
                session.Authorize(request);
                Assert.That(request.Headers.Authorization.Scheme, Is.EqualTo("Bearer"));
                Assert.That(request.Headers.Authorization.Parameter, Is.EqualTo(api.MetaSession));
                Assert.That(request.Headers.Authorization.Parameter, Does.Not.Contain("provider"));
            }
        }

        [Test]
        public void StateMismatchStopsBeforeExchangeAndDoesNotStoreSession()
        {
            var api = new RecordingAuthApi();
            var listenerFactory = new FakeListenerFactory(api, true);
            var session = new MetaSessionState();
            var coordinator = new DesktopLoginCoordinator(
                api,
                listenerFactory,
                new RecordingBrowser(),
                session);

            Assert.ThrowsAsync<DesktopAuthException>(async () =>
                await coordinator.SignInAsync(CancellationToken.None));

            Assert.That(api.ExchangeCalled, Is.False);
            Assert.That(session.IsAuthenticated, Is.False);
            Assert.That(listenerFactory.Listener.Disposed, Is.True);
        }

        private sealed class RecordingAuthApi : IDesktopAuthApi
        {
            public readonly Uri AuthorizationUrl =
                new Uri("https://accounts.google.com/o/oauth2/v2/auth?opaque=1");
            public readonly string MetaSession =
                "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm";

            public string StartState { get; private set; }
            public string StartChallenge { get; private set; }
            public string ExchangeState { get; private set; }
            public string ExchangeVerifier { get; private set; }
            public bool ExchangeCalled { get; private set; }

            public Task<DesktopAuthStart> StartAsync(
                Uri loopbackRedirectUri,
                string state,
                string codeChallenge,
                CancellationToken cancellationToken)
            {
                Assert.That(loopbackRedirectUri.Host, Is.EqualTo("127.0.0.1"));
                Assert.That(loopbackRedirectUri.Port, Is.InRange(49152, 65535));
                Assert.That(loopbackRedirectUri.AbsolutePath, Is.EqualTo("/callback"));
                StartState = state;
                StartChallenge = codeChallenge;
                return Task.FromResult(new DesktopAuthStart(
                    AuthorizationUrl,
                    DateTimeOffset.UtcNow.AddMinutes(2)));
            }

            public Task<MetaSessionIssued> ExchangeAsync(
                string handoffCode,
                string state,
                string codeVerifier,
                CancellationToken cancellationToken)
            {
                ExchangeCalled = true;
                Assert.That(handoffCode, Is.EqualTo(
                    "hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh"));
                ExchangeState = state;
                ExchangeVerifier = codeVerifier;
                return Task.FromResult(new MetaSessionIssued(
                    MetaSession,
                    DateTimeOffset.UtcNow.AddHours(1)));
            }
        }

        private sealed class FakeListenerFactory : ILoopbackAuthListenerFactory
        {
            public FakeListenerFactory(RecordingAuthApi api, bool mismatch)
            {
                Listener = new FakeListener(api, mismatch);
            }

            public FakeListener Listener { get; }

            public ILoopbackAuthListener Listen()
            {
                return Listener;
            }
        }

        private sealed class FakeListener : ILoopbackAuthListener
        {
            private readonly RecordingAuthApi api;
            private readonly bool mismatch;

            public FakeListener(RecordingAuthApi api, bool mismatch)
            {
                this.api = api;
                this.mismatch = mismatch;
            }

            public Uri CallbackUri { get; } =
                new Uri("http://127.0.0.1:49152/callback");
            public bool Disposed { get; private set; }

            public Task<LoopbackAuthCallback> WaitAsync(CancellationToken cancellationToken)
            {
                return Task.FromResult(new LoopbackAuthCallback(
                    "hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh",
                    mismatch
                        ? "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                        : api.StartState));
            }

            public void Dispose()
            {
                Disposed = true;
            }
        }

        private sealed class RecordingBrowser : ISystemBrowser
        {
            public Uri Opened { get; private set; }

            public void Open(Uri uri)
            {
                Opened = uri;
            }
        }
    }
}
