using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public readonly struct StandaloneOAuthCallback
    {
        public readonly string Code;
        public readonly string State;

        public StandaloneOAuthCallback(string code, string state)
        {
            Code = code ?? string.Empty;
            State = state ?? string.Empty;
        }
    }

    public sealed class StandaloneOAuthLoopbackListener : IDisposable
    {
        public const string CallbackPath = "/release0/oauth/callback";

        private readonly TcpListener listener;
        private readonly string expectedState;
        private bool disposed;

        private StandaloneOAuthLoopbackListener(TcpListener listener, string expectedState)
        {
            this.listener = listener;
            this.expectedState = expectedState;
            Port = ((IPEndPoint)listener.LocalEndpoint).Port;
            CallbackUri = $"http://127.0.0.1:{Port}{CallbackPath}";
        }

        public int Port { get; }
        public string CallbackUri { get; }

        public static StandaloneOAuthLoopbackListener Start(string expectedState)
        {
            if (!StandaloneOAuthClient.IsAllowedState(expectedState))
            {
                throw new ArgumentException(
                    "Expected state must be URL-safe and 128 characters or shorter",
                    nameof(expectedState));
            }

            TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            return new StandaloneOAuthLoopbackListener(listener, expectedState);
        }

        public async Task<StandaloneOAuthCallback> WaitForCallbackAsync(TimeSpan timeout)
        {
            ThrowIfDisposed();
            if (timeout <= TimeSpan.Zero)
            {
                throw new ArgumentOutOfRangeException(nameof(timeout));
            }

            Task<TcpClient> acceptTask = listener.AcceptTcpClientAsync();
            Task completedTask = await Task.WhenAny(
                acceptTask,
                Task.Delay(timeout)).ConfigureAwait(false);
            if (completedTask != acceptTask)
            {
                Dispose();
                _ = acceptTask.ContinueWith(
                    task => _ = task.Exception,
                    TaskContinuationOptions.OnlyOnFaulted);
                throw new TimeoutException("Standalone OAuth callback timed out");
            }

            using TcpClient client = await acceptTask.ConfigureAwait(false);
            return await HandleClientAsync(client).ConfigureAwait(false);
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
            listener.Stop();
        }

        private async Task<StandaloneOAuthCallback> HandleClientAsync(TcpClient client)
        {
            using NetworkStream stream = client.GetStream();
            using StreamReader reader = new StreamReader(
                stream,
                Encoding.ASCII,
                detectEncodingFromByteOrderMarks: false,
                bufferSize: 1024,
                leaveOpen: true);
            string requestLine = await reader.ReadLineAsync().ConfigureAwait(false);
            if (!TryParseCallback(requestLine, out StandaloneOAuthCallback callback))
            {
                await WriteResponseAsync(stream, 400, "Bad Request", FailureHtml)
                    .ConfigureAwait(false);
                throw new InvalidOperationException("Invalid Standalone OAuth callback");
            }

            await WriteResponseAsync(stream, 200, "OK", SuccessHtml).ConfigureAwait(false);
            return callback;
        }

        private bool TryParseCallback(string requestLine, out StandaloneOAuthCallback callback)
        {
            callback = default;
            if (string.IsNullOrWhiteSpace(requestLine))
            {
                return false;
            }

            string[] parts = requestLine.Split(' ');
            if (parts.Length < 2 || parts[0] != "GET" || !parts[1].StartsWith("/", StringComparison.Ordinal))
            {
                return false;
            }

            Uri uri;
            try
            {
                uri = new Uri("http://127.0.0.1" + parts[1]);
            }
            catch (UriFormatException)
            {
                return false;
            }

            if (uri.AbsolutePath != CallbackPath)
            {
                return false;
            }

            string code = QueryValue(uri.Query, "code");
            string state = QueryValue(uri.Query, "state");
            if (string.IsNullOrWhiteSpace(code) || state != expectedState)
            {
                return false;
            }

            callback = new StandaloneOAuthCallback(code, state);
            return true;
        }

        private static string QueryValue(string query, string name)
        {
            if (string.IsNullOrEmpty(query))
            {
                return string.Empty;
            }

            string rawQuery = query[0] == '?' ? query.Substring(1) : query;
            foreach (string pair in rawQuery.Split('&'))
            {
                if (string.IsNullOrEmpty(pair))
                {
                    continue;
                }

                string[] parts = pair.Split(new[] { '=' }, 2);
                if (parts.Length == 2 && parts[0] == name)
                {
                    return Uri.UnescapeDataString(parts[1].Replace("+", " "));
                }
            }

            return string.Empty;
        }

        private static async Task WriteResponseAsync(
            NetworkStream stream,
            int statusCode,
            string reason,
            string body)
        {
            byte[] bodyBytes = Encoding.UTF8.GetBytes(body);
            string header =
                $"HTTP/1.1 {statusCode} {reason}\r\n" +
                "Content-Type: text/html; charset=utf-8\r\n" +
                $"Content-Length: {bodyBytes.Length}\r\n" +
                "Connection: close\r\n\r\n";
            byte[] headerBytes = Encoding.ASCII.GetBytes(header);
            await stream.WriteAsync(headerBytes, 0, headerBytes.Length).ConfigureAwait(false);
            await stream.WriteAsync(bodyBytes, 0, bodyBytes.Length).ConfigureAwait(false);
        }

        private void ThrowIfDisposed()
        {
            if (disposed)
            {
                throw new ObjectDisposedException(nameof(StandaloneOAuthLoopbackListener));
            }
        }

        private const string SuccessHtml =
            "<!doctype html><html><body>로그인이 완료되었습니다. 게임으로 돌아가세요.</body></html>";

        private const string FailureHtml =
            "<!doctype html><html><body>로그인 처리에 실패했습니다. 게임에서 다시 시도해주세요.</body></html>";
    }
}
