using System;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public readonly struct MetaHttpResult<T>
    {
        public readonly bool Succeeded;
        public readonly int StatusCode;
        public readonly T Value;
        public readonly string Error;

        private MetaHttpResult(bool succeeded, int statusCode, T value, string error)
        {
            Succeeded = succeeded;
            StatusCode = Math.Max(0, statusCode);
            Value = value;
            Error = error ?? string.Empty;
        }

        public static MetaHttpResult<T> Success(T value, int statusCode)
        {
            return new MetaHttpResult<T>(true, statusCode, value, string.Empty);
        }

        public static MetaHttpResult<T> Failure(int statusCode, string error)
        {
            return new MetaHttpResult<T>(false, statusCode, default, error);
        }
    }

    public sealed class MetaHttpSessionClient : IMetaSessionClient, IDisposable
    {
        private readonly MetaApiClient apiClient;
        private readonly HttpClient httpClient;
        private bool disposed;

        public MetaHttpSessionClient()
            : this(new CookieContainer())
        {
        }

        public MetaHttpSessionClient(CookieContainer cookies)
            : this(CreateHttpClient(cookies), cookies)
        {
        }

        public MetaHttpSessionClient(HttpMessageHandler handler)
            : this(new HttpClient(handler), new CookieContainer())
        {
        }

        private MetaHttpSessionClient(HttpClient httpClient, CookieContainer cookies)
        {
            this.httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
            Cookies = cookies ?? throw new ArgumentNullException(nameof(cookies));
            apiClient = new MetaApiClient();
        }

        public CookieContainer Cookies { get; }

        public Task<MetaHttpResult<MetaAuthSession>> ExchangeStandaloneCodeAsync(
            string metaBaseUrl,
            string code,
            string state)
        {
            return SendAuthSessionRequestAsync(
                metaBaseUrl,
                apiClient.BuildStandaloneExchangeRequest(code, state),
                includeCsrf: false);
        }

        public Task<MetaHttpResult<MetaAuthSession>> GetSessionAsync(string metaBaseUrl)
        {
            return SendAuthSessionRequestAsync(
                metaBaseUrl,
                apiClient.BuildSessionRequest(),
                includeCsrf: false);
        }

        public Task<MetaHttpResult<MetaNicknameAvailability>> CheckNicknameAsync(
            string metaBaseUrl,
            string nickname)
        {
            return SendNicknameAvailabilityRequestAsync(
                metaBaseUrl,
                apiClient.BuildNicknameCheckRequest(nickname),
                includeCsrf: true);
        }

        public Task<MetaHttpResult<MetaAuthSession>> UpdateNicknameAsync(
            string metaBaseUrl,
            string nickname)
        {
            return SendAuthSessionRequestAsync(
                metaBaseUrl,
                apiClient.BuildNicknameRequest(nickname),
                includeCsrf: true);
        }

        public Task<MetaHttpResult<MetaAdmissionOutcome>> EnterAdmissionAsync(
            string metaBaseUrl)
        {
            return EnterAdmissionAsync(metaBaseUrl, string.Empty);
        }

        public Task<MetaHttpResult<MetaAdmissionOutcome>> EnterAdmissionAsync(
            string metaBaseUrl,
            string inviteCode)
        {
            return SendAdmissionRequestAsync(
                metaBaseUrl,
                apiClient.BuildAdmissionRequest(inviteCode),
                includeCsrf: true);
        }

        public Task<MetaHttpResult<MetaAdmissionOutcome>> GetAdmissionStatusAsync(
            string metaBaseUrl)
        {
            return SendAdmissionRequestAsync(
                metaBaseUrl,
                apiClient.BuildAdmissionStatusRequest(),
                includeCsrf: false);
        }

        public Task<MetaHttpResult<MetaAdmissionOutcome>> GetQueueStatusAsync(
            string metaBaseUrl,
            string queueToken)
        {
            return SendAdmissionRequestAsync(
                metaBaseUrl,
                apiClient.BuildQueueStatusRequest(queueToken),
                includeCsrf: false);
        }

        public Task<MetaHttpResult<MetaAdmissionOutcome>> CancelQueueAsync(
            string metaBaseUrl,
            string queueToken)
        {
            return SendAdmissionRequestAsync(
                metaBaseUrl,
                apiClient.BuildQueueCancelRequest(queueToken),
                includeCsrf: true);
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
            httpClient.Dispose();
        }

        private async Task<MetaHttpResult<MetaAuthSession>> SendAuthSessionRequestAsync(
            string metaBaseUrl,
            MetaApiRequest request,
            bool includeCsrf)
        {
            MetaRawHttpResult raw = await SendRequestAsync(
                metaBaseUrl,
                request,
                includeCsrf).ConfigureAwait(false);
            if (!raw.Succeeded)
            {
                return MetaHttpResult<MetaAuthSession>.Failure(raw.StatusCode, raw.Error);
            }

            if (!MetaApiClient.TryParseAuthSessionJson(
                    raw.Body,
                    out MetaAuthSession session,
                    out string error))
            {
                return MetaHttpResult<MetaAuthSession>.Failure(raw.StatusCode, error);
            }

            return MetaHttpResult<MetaAuthSession>.Success(session, raw.StatusCode);
        }

        private async Task<MetaHttpResult<MetaNicknameAvailability>>
            SendNicknameAvailabilityRequestAsync(
                string metaBaseUrl,
                MetaApiRequest request,
                bool includeCsrf)
        {
            MetaRawHttpResult raw = await SendRequestAsync(
                metaBaseUrl,
                request,
                includeCsrf).ConfigureAwait(false);
            if (!raw.Succeeded)
            {
                return MetaHttpResult<MetaNicknameAvailability>.Failure(
                    raw.StatusCode,
                    raw.Error);
            }

            if (!MetaApiClient.TryParseNicknameAvailabilityJson(
                    raw.Body,
                    out MetaNicknameAvailability availability,
                    out string error))
            {
                return MetaHttpResult<MetaNicknameAvailability>.Failure(raw.StatusCode, error);
            }

            return MetaHttpResult<MetaNicknameAvailability>.Success(
                availability,
                raw.StatusCode);
        }

        private async Task<MetaHttpResult<MetaAdmissionOutcome>> SendAdmissionRequestAsync(
            string metaBaseUrl,
            MetaApiRequest request,
            bool includeCsrf)
        {
            MetaRawHttpResult raw = await SendRequestAsync(
                metaBaseUrl,
                request,
                includeCsrf).ConfigureAwait(false);
            if (!raw.Succeeded)
            {
                return MetaHttpResult<MetaAdmissionOutcome>.Failure(raw.StatusCode, raw.Error);
            }

            if (!MetaApiClient.TryParseAdmissionJson(
                    raw.Body,
                    out MetaQueueStatus status,
                    out MetaAdmissionResult admission,
                    out string error))
            {
                return MetaHttpResult<MetaAdmissionOutcome>.Failure(raw.StatusCode, error);
            }

            return MetaHttpResult<MetaAdmissionOutcome>.Success(
                new MetaAdmissionOutcome(status, admission),
                raw.StatusCode);
        }

        private async Task<MetaRawHttpResult> SendRequestAsync(
            string metaBaseUrl,
            MetaApiRequest request,
            bool includeCsrf)
        {
            MetaCsrfToken csrfToken = default;
            if (includeCsrf)
            {
                MetaHttpResult<MetaCsrfToken> csrf =
                    await FetchCsrfAsync(metaBaseUrl).ConfigureAwait(false);
                if (!csrf.Succeeded)
                {
                    return MetaRawHttpResult.Failure(csrf.StatusCode, csrf.Error);
                }

                csrfToken = csrf.Value;
            }

            if (!TryBuildUri(metaBaseUrl, request.Path, out Uri uri, out string uriError))
            {
                return MetaRawHttpResult.Failure(0, uriError);
            }

            using HttpRequestMessage message =
                new HttpRequestMessage(new HttpMethod(request.Method), uri);
            if (!string.IsNullOrEmpty(request.BodyJson))
            {
                message.Content = new StringContent(
                    request.BodyJson,
                    Encoding.UTF8,
                    "application/json");
            }

            if (includeCsrf)
            {
                message.Headers.TryAddWithoutValidation(
                    csrfToken.HeaderName,
                    csrfToken.Token);
            }

            try
            {
                using HttpResponseMessage response =
                    await httpClient.SendAsync(message).ConfigureAwait(false);
                string body = response.Content == null ?
                    string.Empty :
                    await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                int statusCode = (int)response.StatusCode;
                if (!response.IsSuccessStatusCode)
                {
                    return MetaRawHttpResult.Failure(
                        statusCode,
                        "HTTP " + statusCode);
                }

                return MetaRawHttpResult.Success(statusCode, body);
            }
            catch (HttpRequestException exception)
            {
                return MetaRawHttpResult.Failure(0, exception.Message);
            }
            catch (TaskCanceledException)
            {
                return MetaRawHttpResult.Failure(0, "request timed out");
            }
        }

        private async Task<MetaHttpResult<MetaCsrfToken>> FetchCsrfAsync(
            string metaBaseUrl)
        {
            if (!TryBuildUri(
                    metaBaseUrl,
                    apiClient.BuildCsrfRequest().Path,
                    out Uri uri,
                    out string uriError))
            {
                return MetaHttpResult<MetaCsrfToken>.Failure(0, uriError);
            }

            try
            {
                using HttpResponseMessage response =
                    await httpClient.GetAsync(uri).ConfigureAwait(false);
                string body = response.Content == null ?
                    string.Empty :
                    await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                int statusCode = (int)response.StatusCode;
                if (!response.IsSuccessStatusCode)
                {
                    return MetaHttpResult<MetaCsrfToken>.Failure(
                        statusCode,
                        "HTTP " + statusCode);
                }

                if (!TryParseCsrfJson(body, out MetaCsrfToken token, out string error))
                {
                    return MetaHttpResult<MetaCsrfToken>.Failure(statusCode, error);
                }

                return MetaHttpResult<MetaCsrfToken>.Success(token, statusCode);
            }
            catch (HttpRequestException exception)
            {
                return MetaHttpResult<MetaCsrfToken>.Failure(0, exception.Message);
            }
            catch (TaskCanceledException)
            {
                return MetaHttpResult<MetaCsrfToken>.Failure(0, "request timed out");
            }
        }

        private static HttpClient CreateHttpClient(CookieContainer cookies)
        {
            return new HttpClient(new HttpClientHandler
            {
                CookieContainer = cookies ?? throw new ArgumentNullException(nameof(cookies)),
                UseCookies = true
            });
        }

        private static bool TryBuildUri(
            string metaBaseUrl,
            string path,
            out Uri uri,
            out string error)
        {
            uri = null;
            error = string.Empty;
            if (string.IsNullOrWhiteSpace(metaBaseUrl))
            {
                error = "meta base URL is required";
                return false;
            }

            if (string.IsNullOrWhiteSpace(path) ||
                !path.StartsWith("/", StringComparison.Ordinal))
            {
                error = "meta request path is invalid";
                return false;
            }

            return Uri.TryCreate(
                metaBaseUrl.TrimEnd('/') + path,
                UriKind.Absolute,
                out uri);
        }

        private static bool TryParseCsrfJson(
            string json,
            out MetaCsrfToken token,
            out string error)
        {
            token = default;
            error = string.Empty;
            if (string.IsNullOrWhiteSpace(json))
            {
                error = "empty csrf response";
                return false;
            }

            MetaCsrfResponse response;
            try
            {
                response = JsonUtility.FromJson<MetaCsrfResponse>(json);
            }
            catch (ArgumentException exception)
            {
                error = exception.Message;
                return false;
            }

            if (response == null ||
                string.IsNullOrWhiteSpace(response.headerName) ||
                string.IsNullOrWhiteSpace(response.token))
            {
                error = "invalid csrf response";
                return false;
            }

            token = new MetaCsrfToken(response.headerName, response.token);
            return true;
        }

        private readonly struct MetaRawHttpResult
        {
            public readonly bool Succeeded;
            public readonly int StatusCode;
            public readonly string Body;
            public readonly string Error;

            private MetaRawHttpResult(
                bool succeeded,
                int statusCode,
                string body,
                string error)
            {
                Succeeded = succeeded;
                StatusCode = Math.Max(0, statusCode);
                Body = body ?? string.Empty;
                Error = error ?? string.Empty;
            }

            public static MetaRawHttpResult Success(int statusCode, string body)
            {
                return new MetaRawHttpResult(true, statusCode, body, string.Empty);
            }

            public static MetaRawHttpResult Failure(int statusCode, string error)
            {
                return new MetaRawHttpResult(false, statusCode, string.Empty, error);
            }
        }

        private readonly struct MetaCsrfToken
        {
            public readonly string HeaderName;
            public readonly string Token;

            public MetaCsrfToken(string headerName, string token)
            {
                HeaderName = headerName ?? string.Empty;
                Token = token ?? string.Empty;
            }
        }

        [Serializable]
        private sealed class MetaCsrfResponse
        {
            public string headerName;
            public string parameterName;
            public string token;
        }
    }
}
