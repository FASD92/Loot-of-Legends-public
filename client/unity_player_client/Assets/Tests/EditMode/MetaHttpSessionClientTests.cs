using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class MetaHttpSessionClientTests
    {
        [Test]
        public void OwnsIndependentCookieContainer()
        {
            using MetaHttpSessionClient first = new MetaHttpSessionClient();
            using MetaHttpSessionClient second = new MetaHttpSessionClient();

            Assert.NotNull(first.Cookies);
            Assert.NotNull(second.Cookies);
            Assert.AreNotSame(first.Cookies, second.Cookies);
        }

        [Test]
        public void ExchangeStandaloneCodePostsJsonToMetaBaseUrl()
        {
            RecordingHandler handler = new RecordingHandler(
                new HttpResponseMessage(System.Net.HttpStatusCode.OK)
                {
                    Content = new StringContent(
                        "{\"authenticated\":true,\"accountId\":7,\"nicknameRequired\":false,\"nickname\":\"Player7\"}")
                });
            using MetaHttpSessionClient client = new MetaHttpSessionClient(handler);

            MetaHttpResult<MetaAuthSession> result =
                client.ExchangeStandaloneCodeAsync(
                    "https://meta.example.com/",
                    "code-1",
                    "state-1").Result;

            Assert.True(result.Succeeded);
            Assert.AreEqual(7L, result.Value.AccountId);
            Assert.AreEqual(1, handler.Requests.Count);
            Assert.AreEqual(HttpMethod.Post, handler.Requests[0].Method);
            Assert.AreEqual(
                "https://meta.example.com/api/release0/auth/standalone/exchange",
                handler.Requests[0].Uri.ToString());
            StringAssert.Contains("\"code\":\"code-1\"", handler.Requests[0].Body);
            StringAssert.Contains("\"state\":\"state-1\"", handler.Requests[0].Body);
        }

        [Test]
        public void AdmissionPostFetchesCsrfAndSendsHeader()
        {
            RecordingHandler handler = new RecordingHandler(
                new HttpResponseMessage(System.Net.HttpStatusCode.OK)
                {
                    Content = new StringContent(
                        "{\"headerName\":\"X-CSRF-TOKEN\",\"parameterName\":\"_csrf\",\"token\":\"csrf-1\"}")
                },
                new HttpResponseMessage(System.Net.HttpStatusCode.OK)
                {
                    Content = new StringContent(
                        "{\"status\":\"Queued\",\"queueToken\":\"queue-token-1\",\"position\":1}")
                });
            using MetaHttpSessionClient client = new MetaHttpSessionClient(handler);

            MetaHttpResult<MetaAdmissionOutcome> result =
                client.EnterAdmissionAsync(
                    "https://meta.example.com",
                    "portfolio-2026").Result;

            Assert.True(result.Succeeded);
            Assert.AreEqual(MetaQueueState.Queued, result.Value.Status.State);
            Assert.AreEqual("queue-token-1", result.Value.Status.QueueToken);
            Assert.AreEqual(2, handler.Requests.Count);
            Assert.AreEqual(
                "https://meta.example.com/api/release0/auth/csrf",
                handler.Requests[0].Uri.ToString());
            Assert.AreEqual(
                "https://meta.example.com/api/release0/admission/enter",
                handler.Requests[1].Uri.ToString());
            Assert.AreEqual("csrf-1", handler.Requests[1].Header("X-CSRF-TOKEN"));
            StringAssert.Contains("\"inviteCode\":\"portfolio-2026\"", handler.Requests[1].Body);
        }

        [Test]
        public void NicknameCheckFetchesCsrfAndPostsJson()
        {
            RecordingHandler handler = new RecordingHandler(
                new HttpResponseMessage(System.Net.HttpStatusCode.OK)
                {
                    Content = new StringContent(
                        "{\"headerName\":\"X-CSRF-TOKEN\",\"parameterName\":\"_csrf\",\"token\":\"csrf-1\"}")
                },
                new HttpResponseMessage(System.Net.HttpStatusCode.OK)
                {
                    Content = new StringContent(
                        "{\"available\":true,\"message\":\"사용 가능한 플레이어 이름입니다\"}")
                });
            using MetaHttpSessionClient client = new MetaHttpSessionClient(handler);

            MetaHttpResult<MetaNicknameAvailability> result =
                client.CheckNicknameAsync(
                    "https://meta.example.com",
                    "Ready26").Result;

            Assert.True(result.Succeeded);
            Assert.True(result.Value.Available);
            Assert.AreEqual("사용 가능한 플레이어 이름입니다", result.Value.Message);
            Assert.AreEqual(2, handler.Requests.Count);
            Assert.AreEqual(
                "https://meta.example.com/api/release0/auth/csrf",
                handler.Requests[0].Uri.ToString());
            Assert.AreEqual(
                "https://meta.example.com/api/release0/auth/nickname/check",
                handler.Requests[1].Uri.ToString());
            Assert.AreEqual("csrf-1", handler.Requests[1].Header("X-CSRF-TOKEN"));
            StringAssert.Contains("\"nickname\":\"Ready26\"", handler.Requests[1].Body);
        }

        private sealed class RecordingHandler : HttpMessageHandler
        {
            private readonly Queue<HttpResponseMessage> responses =
                new Queue<HttpResponseMessage>();

            public RecordingHandler(params HttpResponseMessage[] responses)
            {
                foreach (HttpResponseMessage response in responses)
                {
                    this.responses.Enqueue(response);
                }
            }

            public List<RecordedRequest> Requests { get; } =
                new List<RecordedRequest>();

            protected override Task<HttpResponseMessage> SendAsync(
                HttpRequestMessage request,
                CancellationToken cancellationToken)
            {
                Requests.Add(RecordedRequest.From(request));
                if (responses.Count == 0)
                {
                    throw new InvalidOperationException("missing response");
                }

                return Task.FromResult(responses.Dequeue());
            }
        }

        private readonly struct RecordedRequest
        {
            private readonly Dictionary<string, string> headers;

            private RecordedRequest(
                HttpMethod method,
                Uri uri,
                string body,
                Dictionary<string, string> headers)
            {
                Method = method;
                Uri = uri;
                Body = body;
                this.headers = headers;
            }

            public HttpMethod Method { get; }
            public Uri Uri { get; }
            public string Body { get; }

            public string Header(string name)
            {
                return headers.TryGetValue(name, out string value) ? value : string.Empty;
            }

            public static RecordedRequest From(HttpRequestMessage request)
            {
                Dictionary<string, string> headers =
                    new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (KeyValuePair<string, IEnumerable<string>> header in request.Headers)
                {
                    headers[header.Key] = string.Join(",", header.Value);
                }

                return new RecordedRequest(
                    request.Method,
                    request.RequestUri,
                    request.Content == null ?
                        string.Empty :
                        request.Content.ReadAsStringAsync().Result,
                    headers);
            }
        }
    }
}
