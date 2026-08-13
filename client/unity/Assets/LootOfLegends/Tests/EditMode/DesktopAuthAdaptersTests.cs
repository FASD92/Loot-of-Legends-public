using System;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Session;
using NUnit.Framework;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class DesktopAuthAdaptersTests
    {
        private const string State = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        private const string Challenge = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        private const string Handoff = "ccccccccccccccccccccccccccccccccccccccccccc";
        private const string Session = "ddddddddddddddddddddddddddddddddddddddddddd";

        [Test]
        public async Task HttpApiUsesOnlyFrozenHandoffAndMetaSessionFields()
        {
            var handler = new RecordingHandler();
            var api = new DesktopAuthHttpApi(
                new HttpClient(handler),
                new Uri("https://meta.example.invalid/"));

            DesktopAuthStart started = await api.StartAsync(
                new Uri("http://127.0.0.1:49152/callback"),
                State,
                Challenge,
                CancellationToken.None);
            MetaSessionIssued issued = await api.ExchangeAsync(
                Handoff,
                State,
                Challenge,
                CancellationToken.None);

            Assert.That(started.AuthorizationUrl.Host, Is.EqualTo("accounts.google.com"));
            Assert.That(issued.MetaSession, Is.EqualTo(Session));
            Assert.That(handler.StartBody, Does.Contain("loopbackRedirectUri"));
            Assert.That(handler.StartBody, Does.Contain("codeChallenge"));
            Assert.That(handler.ExchangeBody, Does.Contain("handoffCode"));
            Assert.That(handler.StartBody + handler.ExchangeBody,
                Does.Not.Contain("accessToken"));
            Assert.That(handler.StartBody + handler.ExchangeBody,
                Does.Not.Contain("idToken"));
        }

        [Test]
        public async Task LoopbackListenerBindsStrictRandomHighPortAndReadsOneCallback()
        {
            var factory = new TcpLoopbackAuthListenerFactory();
            using (ILoopbackAuthListener listener = factory.Listen())
            using (var http = new HttpClient())
            {
                Assert.That(listener.CallbackUri.Scheme, Is.EqualTo("http"));
                Assert.That(listener.CallbackUri.Host, Is.EqualTo("127.0.0.1"));
                Assert.That(listener.CallbackUri.Port, Is.InRange(49152, 65535));
                Assert.That(listener.CallbackUri.AbsolutePath, Is.EqualTo("/callback"));

                Task<LoopbackAuthCallback> waiting =
                    listener.WaitAsync(CancellationToken.None);
                HttpResponseMessage response = await http.GetAsync(
                    listener.CallbackUri + "?code=" + Handoff + "&state=" + State);
                LoopbackAuthCallback callback = await waiting;

                Assert.That(response.StatusCode, Is.EqualTo(HttpStatusCode.OK));
                Assert.That(callback.HandoffCode, Is.EqualTo(Handoff));
                Assert.That(callback.State, Is.EqualTo(State));
            }
        }

        private sealed class RecordingHandler : HttpMessageHandler
        {
            public string StartBody { get; private set; }
            public string ExchangeBody { get; private set; }

            protected override async Task<HttpResponseMessage> SendAsync(
                HttpRequestMessage request,
                CancellationToken cancellationToken)
            {
                string body = await request.Content.ReadAsStringAsync();
                if (request.RequestUri.AbsolutePath.EndsWith("/attempts"))
                {
                    StartBody = body;
                    return Json(
                        "{\"authorizationUrl\":\"https://accounts.google.com/o/oauth2/v2/auth?opaque=1\",\"expiresAt\":\"2030-08-10T00:02:00Z\"}");
                }
                ExchangeBody = body;
                return Json(
                    "{\"metaSession\":\"" + Session +
                    "\",\"expiresAt\":\"2030-08-10T01:00:00Z\"}");
            }

            private static HttpResponseMessage Json(string body)
            {
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(body, Encoding.UTF8, "application/json")
                };
            }
        }
    }
}
