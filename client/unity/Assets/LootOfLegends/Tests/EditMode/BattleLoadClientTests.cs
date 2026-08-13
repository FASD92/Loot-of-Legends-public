using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.Build;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class BattleLoadClientTests
    {
        [Test]
        public void ProjectUsesPinnedUnityBoundarySettings()
        {
            Assert.That(Application.unityVersion, Is.EqualTo("6000.3.21f1"));
            Assert.That(EditorSettings.serializationMode, Is.EqualTo(SerializationMode.ForceText));
            Assert.That(
                PlayerSettings.GetApiCompatibilityLevel(NamedBuildTarget.Standalone),
                Is.EqualTo(ApiCompatibilityLevel.NET_Standard));
            Assert.That(
                PlayerSettings.GetScriptingBackend(NamedBuildTarget.Standalone),
                Is.EqualTo(ScriptingImplementation.Mono2x));
            Assert.That(
                PlayerSettings.GetArchitecture(NamedBuildTarget.Standalone),
                Is.EqualTo((int)OSArchitecture.ARM64));

            string builder = File.ReadAllText(Path.Combine(
                Application.dataPath,
                "LootOfLegends/Editor/StandaloneDevelopmentBuilder.cs"));
            StringAssert.Contains("ScriptingImplementation.Mono2x", builder);
            StringAssert.Contains("OSArchitecture.ARM64", builder);
            StringAssert.DoesNotContain("ScriptingImplementation.IL2CPP", builder);
            StringAssert.DoesNotContain("BuildWindows", builder);
            StringAssert.DoesNotContain("OSArchitecture.x64ARM64", builder);
        }

        [Test]
        public void CodecMatchesFrozenGoldenContract()
        {
            CollectionAssert.AreEqual(
                GoldenFrame("HostStartRequest"),
                BattleLoadProtocolCodec.EncodeHostStart(1));
            CollectionAssert.AreEqual(
                GoldenFrame("ArenaLoadComplete"),
                BattleLoadProtocolCodec.EncodeArenaLoadComplete(1, 7, 1));

            var response = (BattleCommandResponse)BattleLoadProtocolCodec.DecodeServerFrame(
                GoldenFrame("BattleCommandResponse"));
            Assert.That(response.RequestId, Is.EqualTo(1));
            Assert.That(response.Result, Is.EqualTo(BattleLoadResultCode.Ok));

            var entry = (ArenaLoadEntry)BattleLoadProtocolCodec.DecodeServerFrame(
                GoldenFrame("ArenaLoadEntry"));
            Assert.That(entry.RoomId, Is.EqualTo(7));
            Assert.That(entry.BattleInstanceId, Is.EqualTo(1));

            var start = (ArenaGameplayStart)BattleLoadProtocolCodec.DecodeServerFrame(
                GoldenFrame("ArenaGameplayStart"));
            Assert.That(start.Participants.Select(participant => participant.Nickname),
                Is.EqualTo(new[] { "neo", "trinity" }));

            var cancelled = (ArenaLoadCancelled)BattleLoadProtocolCodec.DecodeServerFrame(
                GoldenFrame("ArenaLoadCancelled"));
            Assert.That(cancelled.Reason, Is.EqualTo(ArenaLoadCancelReason.NotEnoughReady));

            Assert.Throws<BattleLoadProtocolException>(() =>
                BattleLoadProtocolCodec.DecodeServerFrame(new byte[] { 0, 0, 0, 1, 1 }));
        }

        [Test]
        public async Task CommandFacadeSendsAndReturnsCorrelationWithoutSocketReceive()
        {
            var sender = new RecordingSender();
            var correlator = new BattleResponseCorrelator();
            var coordinator = new BattleLoadCoordinator(sender, correlator);

            BattleCommandHandle hostStart = await coordinator.HostStartAsync(CancellationToken.None);
            BattleCommandHandle loadComplete = await coordinator.CompleteArenaLoadAsync(
                7,
                1,
                CancellationToken.None);

            Assert.That(hostStart.RequestId, Is.EqualTo(1));
            Assert.That(loadComplete.RequestId, Is.EqualTo(2));
            Assert.That(hostStart.Completion.IsCompleted, Is.False);
            Assert.That(loadComplete.Completion.IsCompleted, Is.False);
            Assert.That(sender.Frames, Has.Count.EqualTo(2));
            CollectionAssert.AreEqual(GoldenFrame("HostStartRequest"), sender.Frames[0]);
            CollectionAssert.AreEqual(
                BattleLoadProtocolCodec.EncodeArenaLoadComplete(2, 7, 1),
                sender.Frames[1]);
            Assert.That(
                typeof(BattleLoadCoordinator).GetFields(
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic),
                Has.None.Matches<FieldInfo>(field => typeof(Stream).IsAssignableFrom(field.FieldType)));
            Assert.That(
                typeof(BattleLoadCoordinator).GetMethods(
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
                    .Select(method => method.Name),
                Has.None.Matches<string>(name =>
                    name.IndexOf("Read", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    name.IndexOf("Receive", StringComparison.OrdinalIgnoreCase) >= 0));

            var readModel = new BattleLoadReadModel();
            var router = new BattleLoadMessageRouter(correlator, readModel);
            router.OnMessage(new BattleCommandResponse(1, BattleLoadResultCode.Ok));
            router.OnMessage(new BattleCommandResponse(2, BattleLoadResultCode.Ok));
            Assert.That(await hostStart.Completion, Is.EqualTo(BattleLoadResultCode.Ok));
            Assert.That(await loadComplete.Completion, Is.EqualTo(BattleLoadResultCode.Ok));
        }

        [Test]
        public async Task TcpInboundPumpOwnsFrameReadsAndRoutesTypedMessages()
        {
            byte[] response = GoldenFrame("BattleCommandResponse");
            byte[] entry = GoldenFrame("ArenaLoadEntry");
            var bytes = new byte[response.Length + entry.Length];
            Buffer.BlockCopy(response, 0, bytes, 0, response.Length);
            Buffer.BlockCopy(entry, 0, bytes, response.Length, entry.Length);
            var sink = new RecordingSink();
            var router = new TypedServerEventRouter(new ImmediateMainThreadDispatcher());

            using (router.Subscribe((ITcpInboundMessageSink)sink))
            {
                await new TcpInboundPump(new MemoryStream(bytes), router)
                    .RunAsync(CancellationToken.None);
            }

            Assert.That(sink.Messages, Has.Count.EqualTo(2));
            Assert.That(sink.Messages[0], Is.TypeOf<BattleCommandResponse>());
            Assert.That(sink.Messages[1], Is.TypeOf<ArenaLoadEntry>());
        }

        private static byte[] GoldenFrame(string semanticName)
        {
            string path = Path.GetFullPath(Path.Combine(
                Application.dataPath,
                "../../../contracts/protocol/golden/battle-load-v1.json"));
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

        private sealed class RecordingSender : ITcpCommandSender
        {
            public List<byte[]> Frames { get; } = new List<byte[]>();

            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                Frames.Add(frame);
                return Task.CompletedTask;
            }
        }

        private sealed class RecordingSink : ITcpInboundMessageSink
        {
            public List<BattleLoadServerMessage> Messages { get; } =
                new List<BattleLoadServerMessage>();

            public void OnMessage(BattleLoadServerMessage message)
            {
                Messages.Add(message);
            }
        }
    }
}
