using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Session;
using LootOfLegends.Transport;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class SessionProtocolClientTests
    {
        private const string Credential =
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

        [Test]
        public void SessionMessagesMatchFrozenGoldenFrames()
        {
            CollectionAssert.AreEqual(
                GoldenFrame("AuthenticateGameSession"),
                SessionProtocolCodec.EncodeAuthenticateGameSession(1, Credential));

            var welcome = (WelcomeSession)SessionProtocolCodec.DecodeServerFrame(
                GoldenFrame("Welcome"));
            Assert.That(welcome.RequestId, Is.EqualTo(1));
            Assert.That(welcome.SessionId, Is.EqualTo(2));
            Assert.That(welcome.SessionGeneration, Is.EqualTo(3));
            Assert.That(welcome.Nickname, Is.EqualTo("player-one"));

            var rejected = (AuthenticationRejectedSession)
                SessionProtocolCodec.DecodeServerFrame(GoldenFrame("AuthenticationRejected"));
            Assert.That(rejected.Reason,
                Is.EqualTo(AuthenticationRejectedReason.Expired));

            var replaced = (SessionReplaced)
                SessionProtocolCodec.DecodeServerFrame(GoldenFrame("SessionReplaced"));
            Assert.That(replaced.Reason, Is.EqualTo(SessionReplacedReason.SameAccountLogin));
        }

        [Test]
        public async Task TcpPumpRoutesSessionReplacedToSessionReadModel()
        {
            var dispatcher = new ImmediateDispatcher();
            var router = new TypedServerEventRouter(dispatcher);
            var model = new PlayerSessionReadModel();
            model.BeginAuthentication();
            model.Apply(new WelcomeSession(1, 2, 3, 1700000000000, "player-one"));
            using (router.Subscribe((ISessionInboundMessageSink)model))
            using (var stream = new MemoryStream(GoldenFrame("SessionReplaced")))
            {
                await new TcpInboundPump(stream, router).RunAsync(CancellationToken.None);
            }

            Assert.That(model.State, Is.EqualTo(PlayerSessionState.Replaced));
            Assert.That(model.SessionId, Is.Zero);
            Assert.That(model.SessionGeneration, Is.Zero);
        }

        private static byte[] GoldenFrame(string semanticName)
        {
            string path = Path.GetFullPath(Path.Combine(
                Application.dataPath,
                "../../../contracts/protocol/golden/session-auth-v1.json"));
            string contract = File.ReadAllText(path);
            string semanticMarker = $"\"semanticName\": \"{semanticName}\"";
            int semanticOffset = contract.IndexOf(semanticMarker, StringComparison.Ordinal);
            Assert.That(semanticOffset, Is.GreaterThanOrEqualTo(0));
            const string frameMarker = "\"frameHex\": \"";
            int frameOffset = contract.IndexOf(frameMarker, semanticOffset, StringComparison.Ordinal);
            Assert.That(frameOffset, Is.GreaterThanOrEqualTo(0));
            int first = frameOffset + frameMarker.Length;
            int last = contract.IndexOf('"', first);
            string hex = contract.Substring(first, last - first);
            var result = new byte[hex.Length / 2];
            for (int index = 0; index < result.Length; index++)
            {
                result[index] = Convert.ToByte(hex.Substring(index * 2, 2), 16);
            }
            return result;
        }

        private sealed class ImmediateDispatcher : IMainThreadDispatcher
        {
            public void Post(Action action)
            {
                action();
            }
        }
    }
}
