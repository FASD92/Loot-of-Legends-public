using System;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading.Tasks;
using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class StandaloneOAuthLoopbackListenerTests
    {
        [Test]
        public void ListenerBindsRandomLoopbackPortAndBuildsCallbackUri()
        {
            using StandaloneOAuthLoopbackListener listener =
                StandaloneOAuthLoopbackListener.Start("state-1");

            StringAssert.StartsWith("http://127.0.0.1:", listener.CallbackUri);
            StringAssert.EndsWith("/release0/oauth/callback", listener.CallbackUri);
            Assert.Greater(listener.Port, 0);
        }

        [Test]
        public void ListenerRejectsStateThatMetaWouldReject()
        {
            Assert.Throws<ArgumentException>(() =>
                StandaloneOAuthLoopbackListener.Start("state 1"));
        }

        [Test]
        public void WaitForCallbackAcceptsExactPathAndState()
        {
            using StandaloneOAuthLoopbackListener listener =
                StandaloneOAuthLoopbackListener.Start("state-1");
            Task<StandaloneOAuthCallback> waitTask =
                listener.WaitForCallbackAsync(TimeSpan.FromSeconds(2));

            SendGet(listener.Port, "/release0/oauth/callback?code=code-1&state=state-1");

            Assert.True(waitTask.Wait(TimeSpan.FromSeconds(3)));
            Assert.AreEqual("code-1", waitTask.Result.Code);
            Assert.AreEqual("state-1", waitTask.Result.State);
        }

        [Test]
        public void WaitForCallbackRejectsWrongState()
        {
            using StandaloneOAuthLoopbackListener listener =
                StandaloneOAuthLoopbackListener.Start("state-1");
            Task<StandaloneOAuthCallback> waitTask =
                listener.WaitForCallbackAsync(TimeSpan.FromSeconds(2));

            SendGet(listener.Port, "/release0/oauth/callback?code=code-1&state=state-2");

            AggregateException exception =
                Assert.Throws<AggregateException>(() => waitTask.Wait(TimeSpan.FromSeconds(3)));
            Assert.That(exception.InnerException, Is.TypeOf<InvalidOperationException>());
        }

        private static void SendGet(int port, string target)
        {
            using TcpClient client = new TcpClient("127.0.0.1", port);
            using NetworkStream stream = client.GetStream();
            byte[] requestBytes = Encoding.ASCII.GetBytes(
                $"GET {target} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n");
            stream.Write(requestBytes, 0, requestBytes.Length);
            using StreamReader reader = new StreamReader(stream, Encoding.UTF8);
            reader.ReadToEnd();
        }
    }
}
