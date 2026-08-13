using System;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.Collection
{
    public interface ICollectionApi
    {
        Task<CollectionSnapshot> FetchAsync(CancellationToken cancellationToken);
    }

    public sealed class CollectionUnavailableException : Exception
    {
        public CollectionUnavailableException(string message)
            : base(message)
        {
        }

        public CollectionUnavailableException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    public sealed class CollectionHttpApi : ICollectionApi
    {
        private const int MaximumResponseBytes = 64 * 1024;
        private readonly HttpClient http;
        private readonly Uri collectionUri;
        private readonly Action<HttpRequestMessage> authorize;

        public CollectionHttpApi(
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
            collectionUri = new Uri(metaBaseUri, "/api/v1/collection");
        }

        public async Task<CollectionSnapshot> FetchAsync(
            CancellationToken cancellationToken)
        {
            using (var request = new HttpRequestMessage(HttpMethod.Get, collectionUri))
            {
                authorize(request);
                using (HttpResponseMessage response =
                    await http.SendAsync(request, cancellationToken).ConfigureAwait(false))
                {
                    if (!response.IsSuccessStatusCode ||
                        response.Content.Headers.ContentLength > MaximumResponseBytes)
                    {
                        throw new CollectionUnavailableException(
                            "Collection is temporarily unavailable");
                    }
                    string body = await response.Content.ReadAsStringAsync()
                        .ConfigureAwait(false);
                    if (Encoding.UTF8.GetByteCount(body) > MaximumResponseBytes)
                    {
                        throw new CollectionUnavailableException(
                            "Collection response exceeded its bounded size");
                    }
                    try
                    {
                        return CollectionSnapshotCodec.Decode(body);
                    }
                    catch (CollectionProtocolException error)
                    {
                        throw new CollectionUnavailableException(
                            "Collection response was rejected", error);
                    }
                }
            }
        }
    }
}
