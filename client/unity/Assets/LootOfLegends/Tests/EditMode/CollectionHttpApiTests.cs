using System;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Collection;
using NUnit.Framework;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class CollectionHttpApiTests
    {
        [Test]
        public async Task FetchUsesMetaPlayerAuthorizationAndFrozenEndpoint()
        {
            var handler = new RecordingHandler(HttpStatusCode.OK,
                "{\"items\":[{\"itemId\":\"1\",\"quantity\":\"4\",\"value\":\"100\"}]," +
                "\"wallet\":\"300\",\"pendingSettlementCount\":2,\"freshness\":\"Fresh\"}");
            var api = new CollectionHttpApi(
                new HttpClient(handler),
                new Uri("https://meta.example.invalid/"),
                request => request.Headers.Add("Authorization", "Bearer opaque-session"));

            CollectionSnapshot snapshot = await api.FetchAsync(CancellationToken.None);

            Assert.That(handler.Method, Is.EqualTo(HttpMethod.Get));
            Assert.That(handler.Path, Is.EqualTo("/api/v1/collection"));
            Assert.That(handler.Authorization, Is.EqualTo("Bearer opaque-session"));
            Assert.That(snapshot.Items[0].Quantity, Is.EqualTo(4));
            Assert.That(snapshot.PendingSettlementCount, Is.EqualTo(2));
        }

        [Test]
        public void FailureDoesNotExposeResponseBodyOrEndpoint()
        {
            var handler = new RecordingHandler(
                HttpStatusCode.ServiceUnavailable,
                "{\"code\":\"DEPENDENCY_UNAVAILABLE\",\"secret\":\"raw\"}");
            var api = new CollectionHttpApi(
                new HttpClient(handler),
                new Uri("https://meta.example.invalid/"),
                request => request.Headers.Add("Authorization", "Bearer opaque-session"));

            CollectionUnavailableException error = Assert.ThrowsAsync<CollectionUnavailableException>(
                async () => await api.FetchAsync(CancellationToken.None));

            Assert.That(error.Message, Does.Not.Contain("DEPENDENCY_UNAVAILABLE"));
            Assert.That(error.Message, Does.Not.Contain("meta.example.invalid"));
            Assert.That(error.Message, Does.Not.Contain("raw"));
        }

        private sealed class RecordingHandler : HttpMessageHandler
        {
            private readonly HttpStatusCode status;
            private readonly string body;

            public RecordingHandler(HttpStatusCode status, string body)
            {
                this.status = status;
                this.body = body;
            }

            public HttpMethod Method { get; private set; }
            public string Path { get; private set; }
            public string Authorization { get; private set; }

            protected override Task<HttpResponseMessage> SendAsync(
                HttpRequestMessage request,
                CancellationToken cancellationToken)
            {
                Method = request.Method;
                Path = request.RequestUri.AbsolutePath;
                Authorization = request.Headers.Authorization == null
                    ? string.Join(" ", request.Headers.GetValues("Authorization"))
                    : request.Headers.Authorization.ToString();
                return Task.FromResult(new HttpResponseMessage(status)
                {
                    Content = new StringContent(body, Encoding.UTF8, "application/json")
                });
            }
        }
    }
}
