using System;
using System.Threading.Tasks;
using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class StandaloneOAuthLoginFlowTests
    {
        [Test]
        public async Task LoginAsyncOpensBrowserWithLoopbackAndExchangesCallback()
        {
            FakeLoopbackSession loopback =
                new FakeLoopbackSession("http://127.0.0.1:54321/release0/oauth/callback");
            FakeBrowser browser = new FakeBrowser();
            FakeMetaSessionClient metaClient = new FakeMetaSessionClient(
                MetaHttpResult<MetaAuthSession>.Success(
                    new MetaAuthSession(true, 7L, false, "Player7"),
                    200));
            StandaloneOAuthLoginFlow flow = new StandaloneOAuthLoginFlow(
                new FakeLoopbackFactory(loopback),
                browser,
                metaClient,
                TimeSpan.FromSeconds(3.0));

            MetaHttpResult<MetaAuthSession> result =
                await flow.LoginAsync("https://meta.example.com/");

            Assert.True(result.Succeeded);
            StringAssert.StartsWith(
                "https://meta.example.com/api/release0/auth/standalone/start?",
                browser.OpenedUrl);
            StringAssert.Contains(
                "callback=http%3A%2F%2F127.0.0.1%3A54321%2Frelease0%2Foauth%2Fcallback",
                browser.OpenedUrl);
            StringAssert.Contains("state=", browser.OpenedUrl);
            Assert.AreEqual(loopback.ExpectedState, metaClient.State);
            Assert.AreEqual("code-1", metaClient.Code);
            Assert.True(loopback.Disposed);
        }

        private sealed class FakeLoopbackFactory : IStandaloneOAuthLoopbackFactory
        {
            private readonly FakeLoopbackSession session;

            public FakeLoopbackFactory(FakeLoopbackSession session)
            {
                this.session = session;
            }

            public IStandaloneOAuthLoopbackSession Start(string expectedState)
            {
                session.ExpectedState = expectedState;
                return session;
            }
        }

        private sealed class FakeLoopbackSession : IStandaloneOAuthLoopbackSession
        {
            public FakeLoopbackSession(string callbackUri)
            {
                CallbackUri = callbackUri;
            }

            public string CallbackUri { get; }
            public string ExpectedState { get; set; }
            public bool Disposed { get; private set; }

            public Task<StandaloneOAuthCallback> WaitForCallbackAsync(TimeSpan timeout)
            {
                return Task.FromResult(
                    new StandaloneOAuthCallback("code-1", ExpectedState));
            }

            public void Dispose()
            {
                Disposed = true;
            }
        }

        private sealed class FakeBrowser : IExternalUrlOpener
        {
            public string OpenedUrl { get; private set; }

            public void OpenUrl(string url)
            {
                OpenedUrl = url;
            }
        }

        private sealed class FakeMetaSessionClient : IStandaloneMetaSessionClient
        {
            private readonly MetaHttpResult<MetaAuthSession> result;

            public FakeMetaSessionClient(MetaHttpResult<MetaAuthSession> result)
            {
                this.result = result;
            }

            public string MetaBaseUrl { get; private set; }
            public string Code { get; private set; }
            public string State { get; private set; }

            public Task<MetaHttpResult<MetaAuthSession>> ExchangeStandaloneCodeAsync(
                string metaBaseUrl,
                string code,
                string state)
            {
                MetaBaseUrl = metaBaseUrl;
                Code = code;
                State = state;
                return Task.FromResult(result);
            }
        }
    }
}
