using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.Session
{
    public sealed class TcpLoopbackAuthListenerFactory : ILoopbackAuthListenerFactory
    {
        private const int MinimumPort = 49152;
        private const int PortCount = 16384;
        private const int BindAttempts = 32;

        public ILoopbackAuthListener Listen()
        {
            for (int attempt = 0; attempt < BindAttempts; attempt++)
            {
                int port = MinimumPort + RandomOffset();
                var listener = new TcpListener(IPAddress.Loopback, port);
                try
                {
                    listener.Start(1);
                    return new TcpLoopbackAuthListener(listener, port);
                }
                catch (SocketException)
                {
                    listener.Stop();
                }
            }
            throw new DesktopAuthException("No desktop auth loopback port is available");
        }

        private static int RandomOffset()
        {
            var bytes = new byte[2];
            using (RandomNumberGenerator random = RandomNumberGenerator.Create())
            {
                random.GetBytes(bytes);
            }
            return ((bytes[0] << 8) | bytes[1]) % PortCount;
        }

        private sealed class TcpLoopbackAuthListener : ILoopbackAuthListener
        {
            private const int MaximumRequestBytes = 8192;
            private readonly TcpListener listener;
            private bool disposed;

            public TcpLoopbackAuthListener(TcpListener listener, int port)
            {
                this.listener = listener;
                CallbackUri = new Uri("http://127.0.0.1:" + port + "/callback");
            }

            public Uri CallbackUri { get; }

            public async Task<LoopbackAuthCallback> WaitAsync(
                CancellationToken cancellationToken)
            {
                ThrowIfDisposed();
                using (cancellationToken.Register(listener.Stop))
                {
                    TcpClient client;
                    try
                    {
                        client = await listener.AcceptTcpClientAsync().ConfigureAwait(false);
                    }
                    catch (Exception error) when (cancellationToken.IsCancellationRequested)
                    {
                        throw new OperationCanceledException(
                            "Desktop auth callback was cancelled", error, cancellationToken);
                    }

                    using (client)
                    using (NetworkStream stream = client.GetStream())
                    {
                        try
                        {
                            string request = await ReadRequestAsync(
                                stream, cancellationToken).ConfigureAwait(false);
                            LoopbackAuthCallback callback = Parse(request);
                            await WriteResponseAsync(stream, true, cancellationToken)
                                .ConfigureAwait(false);
                            return callback;
                        }
                        catch (DesktopAuthException)
                        {
                            await WriteResponseAsync(stream, false, cancellationToken)
                                .ConfigureAwait(false);
                            throw;
                        }
                    }
                }
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

            private LoopbackAuthCallback Parse(string request)
            {
                string[] lines = request.Split(new[] { "\r\n" }, StringSplitOptions.None);
                if (lines.Length < 2)
                {
                    throw new DesktopAuthException("Loopback callback is incomplete");
                }
                string[] requestLine = lines[0].Split(' ');
                if (requestLine.Length != 3 || requestLine[0] != "GET" ||
                    requestLine[2] != "HTTP/1.1")
                {
                    throw new DesktopAuthException("Loopback callback request line is invalid");
                }
                string expectedHost = CallbackUri.Host + ":" + CallbackUri.Port;
                bool hostMatched = false;
                for (int index = 1; index < lines.Length; index++)
                {
                    if (lines[index].StartsWith("Host:", StringComparison.OrdinalIgnoreCase))
                    {
                        hostMatched = lines[index].Substring(5).Trim() == expectedHost;
                    }
                }
                if (!hostMatched ||
                    !Uri.TryCreate(CallbackUri, requestLine[1], out Uri callback) ||
                    callback.Host != "127.0.0.1" || callback.Port != CallbackUri.Port ||
                    callback.AbsolutePath != "/callback")
                {
                    throw new DesktopAuthException("Loopback callback origin is invalid");
                }

                IDictionary<string, string> query = ParseQuery(callback.Query);
                if (!query.TryGetValue("code", out string code) ||
                    !query.TryGetValue("state", out string state) || query.Count != 2)
                {
                    throw new DesktopAuthException("Loopback callback query is invalid");
                }
                return new LoopbackAuthCallback(code, state);
            }

            private static async Task<string> ReadRequestAsync(
                Stream stream,
                CancellationToken cancellationToken)
            {
                var bytes = new byte[MaximumRequestBytes];
                int count = 0;
                while (count < bytes.Length)
                {
                    int read = await stream.ReadAsync(
                        bytes, count, bytes.Length - count, cancellationToken)
                        .ConfigureAwait(false);
                    if (read == 0)
                    {
                        break;
                    }
                    count += read;
                    if (count >= 4 && EndsHeaders(bytes, count))
                    {
                        return Encoding.ASCII.GetString(bytes, 0, count);
                    }
                }
                throw new DesktopAuthException("Loopback callback headers exceed the limit");
            }

            private static bool EndsHeaders(byte[] bytes, int count)
            {
                for (int index = 3; index < count; index++)
                {
                    if (bytes[index - 3] == '\r' && bytes[index - 2] == '\n' &&
                        bytes[index - 1] == '\r' && bytes[index] == '\n')
                    {
                        return true;
                    }
                }
                return false;
            }

            private static IDictionary<string, string> ParseQuery(string query)
            {
                var values = new Dictionary<string, string>(StringComparer.Ordinal);
                string raw = query.StartsWith("?", StringComparison.Ordinal)
                    ? query.Substring(1)
                    : query;
                foreach (string pair in raw.Split('&'))
                {
                    string[] parts = pair.Split(new[] { '=' }, 2);
                    if (parts.Length != 2)
                    {
                        throw new DesktopAuthException("Loopback callback query is malformed");
                    }
                    string name = WebUtility.UrlDecode(parts[0]);
                    string value = WebUtility.UrlDecode(parts[1]);
                    if (values.ContainsKey(name))
                    {
                        throw new DesktopAuthException("Loopback callback query is duplicated");
                    }
                    values.Add(name, value);
                }
                return values;
            }

            private static async Task WriteResponseAsync(
                Stream stream,
                bool success,
                CancellationToken cancellationToken)
            {
                string body = success
                    ? "Authentication complete. Return to Loot of Legends."
                    : "Authentication callback rejected.";
                byte[] bodyBytes = Encoding.UTF8.GetBytes(body);
                string headers = (success ? "HTTP/1.1 200 OK\r\n" :
                    "HTTP/1.1 400 Bad Request\r\n") +
                    "Content-Type: text/plain; charset=utf-8\r\n" +
                    "Cache-Control: no-store\r\n" +
                    "Connection: close\r\n" +
                    "Content-Length: " + bodyBytes.Length + "\r\n\r\n";
                byte[] headerBytes = Encoding.ASCII.GetBytes(headers);
                await stream.WriteAsync(
                    headerBytes, 0, headerBytes.Length, cancellationToken)
                    .ConfigureAwait(false);
                await stream.WriteAsync(
                    bodyBytes, 0, bodyBytes.Length, cancellationToken)
                    .ConfigureAwait(false);
            }

            private void ThrowIfDisposed()
            {
                if (disposed)
                {
                    throw new ObjectDisposedException(nameof(TcpLoopbackAuthListener));
                }
            }
        }
    }
}
