using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class BattleRecoveryProtocolClientTests
    {
        [Test]
        public void GoldenVectorsDecodeWithExactIdentityAndReason()
        {
            BattleRecoveryNotice resultFailure =
                BattleRecoveryProtocolCodec.DecodeServerFrame(
                    GoldenFrame("ResultGenerationFailed"));
            Assert.That(resultFailure.RoomId, Is.EqualTo(7));
            Assert.That(resultFailure.BattleInstanceId, Is.EqualTo(9));
            Assert.That(resultFailure.Reason,
                Is.EqualTo(BattleRecoveryReason.ResultGenerationFailed));

            BattleRecoveryNotice storagePending =
                BattleRecoveryProtocolCodec.DecodeServerFrame(
                    GoldenFrame("SettlementRecoveryPending"));
            Assert.That(storagePending.RoomId, Is.EqualTo(7));
            Assert.That(storagePending.BattleInstanceId, Is.EqualTo(10));
            Assert.That(storagePending.Reason,
                Is.EqualTo(BattleRecoveryReason.SettlementRecoveryPending));
        }

        [Test]
        public void MalformedVersionReasonIdentityAndLengthAreRejected()
        {
            byte[] valid = GoldenFrame("ResultGenerationFailed");
            AssertRejected(Mutate(valid, frame => frame[4] = 2));
            AssertRejected(Mutate(valid, frame => frame[25] = 3));
            AssertRejected(Mutate(valid, frame => Array.Clear(frame, 9, 8)));
            AssertRejected(Mutate(valid, frame => Array.Clear(frame, 17, 8)));

            var trailing = new byte[valid.Length + 1];
            Buffer.BlockCopy(valid, 0, trailing, 0, valid.Length);
            trailing[3]++;
            AssertRejected(trailing);

            var truncated = new byte[valid.Length - 1];
            Buffer.BlockCopy(valid, 0, truncated, 0, truncated.Length);
            AssertRejected(truncated);
        }

        [Test]
        public async Task TcpPumpRoutesId37ThroughTheSingleReceiveOwner()
        {
            var sink = new RecordingRecoverySink();
            var router = new TypedServerEventRouter(
                new ImmediateMainThreadDispatcher());
            using (router.Subscribe((IBattleRecoveryInboundMessageSink)sink))
            {
                await new TcpInboundPump(
                        new MemoryStream(GoldenFrame("SettlementRecoveryPending")),
                        router)
                    .RunAsync(CancellationToken.None);
            }

            Assert.That(sink.Calls, Is.EqualTo(1));
            Assert.That(sink.Last.Reason,
                Is.EqualTo(BattleRecoveryReason.SettlementRecoveryPending));
        }

        private static void AssertRejected(byte[] frame)
        {
            Assert.Throws<BattleRecoveryProtocolException>(() =>
                BattleRecoveryProtocolCodec.DecodeServerFrame(frame));
        }

        private static byte[] Mutate(byte[] source, Action<byte[]> mutate)
        {
            var copy = (byte[])source.Clone();
            mutate(copy);
            return copy;
        }

        private static byte[] GoldenFrame(string semanticName)
        {
            string path = Path.GetFullPath(Path.Combine(
                Application.dataPath,
                "../../../contracts/protocol/golden/battle-recovery-v1.json"));
            string contract = File.ReadAllText(path);
            string semanticMarker = $"\"semanticName\": \"{semanticName}\"";
            int semanticOffset = contract.IndexOf(semanticMarker, StringComparison.Ordinal);
            Assert.That(semanticOffset, Is.GreaterThanOrEqualTo(0));
            const string frameMarker = "\"frameHex\": \"";
            int frameOffset = contract.IndexOf(
                frameMarker, semanticOffset, StringComparison.Ordinal);
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

        private sealed class RecordingRecoverySink : IBattleRecoveryInboundMessageSink
        {
            public int Calls { get; private set; }
            public BattleRecoveryNotice Last { get; private set; }

            public void OnMessage(BattleRecoveryNotice message)
            {
                Calls++;
                Last = message;
            }
        }
    }
}
