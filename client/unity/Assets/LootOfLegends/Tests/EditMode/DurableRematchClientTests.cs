using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class DurableRematchClientTests
    {
        [Test]
        public void FinalResultAndRoomDetailMatchFrozenGoldenContracts()
        {
            BattleFinalResult result = FinalResultProtocolCodec.DecodeServerFrame(
                GoldenFrame("final-result-v1.json", "MonsterDefeatedFinalResult"));
            Assert.That(result.RoomId, Is.EqualTo(7));
            Assert.That(result.BattleInstanceId, Is.EqualTo(9));
            Assert.That(result.Outcome, Is.EqualTo(FinalResultOutcome.MonsterDefeated));
            Assert.That(result.Entries.Select(entry => entry.SessionId),
                Is.EqualTo(new ulong[] { 1, 2 }));
            Assert.That(result.Entries[0].FinalAssetValue, Is.EqualTo(300));
            Assert.That(result.Entries[0].Rank, Is.EqualTo(1));
            Assert.That(result.Entries[0].IsTop, Is.True);

            BattleFinalResult timeout = FinalResultProtocolCodec.DecodeServerFrame(
                GoldenFrame("final-result-v1.json", "CombatTimeoutFinalResult"));
            Assert.That(timeout.Outcome, Is.EqualTo(FinalResultOutcome.CombatTimeout));
            Assert.That(timeout.Entries.All(entry => entry.Rank == null && !entry.IsTop),
                Is.True);

            RoomDetailProjection room = (RoomDetailProjection)
                LobbyRoomProtocolCodec.DecodeServerFrame(
                    GoldenFrame("lobby-room-v1.json", "RoomDetailProjection"));
            Assert.That(room.RoomId, Is.EqualTo(7));
            Assert.That(room.Members, Has.Count.EqualTo(1));
            Assert.That(room.Members.All(member => !member.Ready), Is.True);

            BattleFinalResult cancelled = FinalResultProtocolCodec.DecodeServerFrame(
                GoldenFrame("final-result-v1.json", "CancelledNoActiveParticipantsFinalResult"));
            Assert.That(cancelled.Outcome,
                Is.EqualTo(FinalResultOutcome.CancelledNoActiveParticipants));
            Assert.That(cancelled.Entries.All(entry => entry.Rank == null && !entry.IsTop),
                Is.True);
        }

        [Test]
        public async Task TcpPumpAppliesFinalResultBeforeAllUnreadyRoomProjection()
        {
            byte[] result = GoldenFrame("final-result-v1.json", "MonsterDefeatedFinalResult");
            byte[] room = GoldenFrame("lobby-room-v1.json", "RoomDetailProjection");
            var bytes = new byte[result.Length + room.Length];
            Buffer.BlockCopy(result, 0, bytes, 0, result.Length);
            Buffer.BlockCopy(room, 0, bytes, result.Length, room.Length);

            var load = new BattleLoadReadModel();
            var lobby = new LobbyRoomReadModel();
            var completion = new BattleResultReadModel();
            var battleRouter = new BattleCompletionRouter(
                new BattleResponseCorrelator(), load, completion);
            battleRouter.OnMessage(new ArenaLoadEntry(7, 9));
            var typedRouter = new TypedServerEventRouter(
                new ImmediateMainThreadDispatcher());

            using (typedRouter.Subscribe((ITcpInboundMessageSink)battleRouter))
            using (typedRouter.Subscribe((ILobbyRoomInboundMessageSink)lobby))
            using (typedRouter.Subscribe((ILobbyRoomInboundMessageSink)battleRouter))
            using (typedRouter.Subscribe((IFinalResultInboundMessageSink)battleRouter))
            {
                await new TcpInboundPump(new MemoryStream(bytes), typedRouter)
                    .RunAsync(CancellationToken.None);
            }

            Assert.That(completion.HasFinalResult, Is.True);
            Assert.That(completion.IsReadyForRematch, Is.True);
            Assert.That(lobby.CurrentRoom.RoomId, Is.EqualTo(7));

            var reverse = new byte[result.Length + room.Length];
            Buffer.BlockCopy(room, 0, reverse, 0, room.Length);
            Buffer.BlockCopy(result, 0, reverse, room.Length, result.Length);
            var reverseCompletion = new BattleResultReadModel();
            var reverseRouter = new BattleCompletionRouter(
                new BattleResponseCorrelator(),
                new BattleLoadReadModel(),
                reverseCompletion);
            reverseRouter.OnMessage(new ArenaLoadEntry(7, 9));
            var reverseTypedRouter = new TypedServerEventRouter(
                new ImmediateMainThreadDispatcher());
            using (reverseTypedRouter.Subscribe((ITcpInboundMessageSink)reverseRouter))
            using (reverseTypedRouter.Subscribe((ILobbyRoomInboundMessageSink)reverseRouter))
            using (reverseTypedRouter.Subscribe((IFinalResultInboundMessageSink)reverseRouter))
            {
                await new TcpInboundPump(new MemoryStream(reverse), reverseTypedRouter)
                    .RunAsync(CancellationToken.None);
            }
            Assert.That(reverseCompletion.HasFinalResult, Is.True);
            Assert.That(reverseCompletion.IsReadyForRematch, Is.False);
        }

        [Test]
        public void CollectionUsesAppliedQuantitiesAndPendingCountWithoutPrediction()
        {
            var model = new CollectionReadModel();
            model.Apply(CollectionSnapshotCodec.Decode(
                "{\"items\":[{\"itemId\":\"1\",\"quantity\":\"5\",\"value\":\"100\"}]," +
                "\"wallet\":\"300\",\"pendingSettlementCount\":2,\"freshness\":\"Fresh\"}"));

            Assert.That(model.Items.Single().Quantity, Is.EqualTo(5));
            Assert.That(model.PendingSettlementCount, Is.EqualTo(2));
            Assert.That(model.Wallet, Is.EqualTo(300));

            model.Apply(CollectionSnapshotCodec.Decode(
                "{\"items\":[{\"itemId\":\"1\",\"quantity\":\"5\",\"value\":\"100\"}]," +
                "\"wallet\":\"300\",\"pendingSettlementCount\":3,\"freshness\":\"Fresh\"}"));
            Assert.That(model.Items.Single().Quantity, Is.EqualTo(5));
            Assert.That(model.PendingSettlementCount, Is.EqualTo(3));

            model.MarkUnavailable();
            Assert.That(model.IsFresh, Is.False);
            Assert.That(model.Items.Single().Quantity, Is.EqualTo(5));
            Assert.Throws<CollectionProtocolException>(() => CollectionSnapshotCodec.Decode(
                "{\"items\":[{\"itemId\":\"1\",\"quantity\":\"0\",\"value\":\"100\"}]," +
                "\"wallet\":\"300\",\"pendingSettlementCount\":1,\"freshness\":\"Fresh\"}"));
        }

        private static byte[] GoldenFrame(string fileName, string semanticName)
        {
            string path = Path.GetFullPath(Path.Combine(
                Application.dataPath,
                "../../../contracts/protocol/golden",
                fileName));
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
    }
}
