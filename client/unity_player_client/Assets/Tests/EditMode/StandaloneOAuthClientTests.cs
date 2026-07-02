using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class StandaloneOAuthClientTests
    {
        [Test]
        public void ClientBuildsStartUrlWithEncodedCallbackAndState()
        {
            string url = StandaloneOAuthClient.BuildStartUrl(
                "https://meta.example.com",
                "http://127.0.0.1:53421/release0/oauth/callback",
                "state-1");

            StringAssert.StartsWith(
                "https://meta.example.com/api/release0/auth/standalone/start?",
                url);
            StringAssert.Contains("callback=", url);
            StringAssert.Contains("state=", url);
            StringAssert.Contains("state-1", url);
        }

        [Test]
        public void ClientRejectsStateThatMetaWouldReject()
        {
            Assert.Throws<System.ArgumentException>(() =>
                StandaloneOAuthClient.BuildStartUrl(
                    "https://meta.example.com",
                    "http://127.0.0.1:53421/release0/oauth/callback",
                    "state 1"));
        }

        [Test]
        public void CreateStateReturnsUrlSafeNonce()
        {
            string state = StandaloneOAuthClient.CreateState();

            Assert.GreaterOrEqual(state.Length, 32);
            StringAssert.DoesNotContain("+", state);
            StringAssert.DoesNotContain("/", state);
            StringAssert.DoesNotContain("=", state);
            StringAssert.DoesNotContain(" ", state);
        }
    }
}
