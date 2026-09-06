using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;
using NUnit.Framework;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class BattleSessionResumeProtocolClientTests
    {
        private const string Credential =
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

        [Test]
        public void ResumeRequestAndSnapshotAppliedUseTheFrozenEnvelope()
        {
            byte[] request = BattleSessionResumeProtocolCodec.EncodeResumeBattleSession(
                7, 11, 13, Credential);

            Assert.That(request.Length, Is.EqualTo(78));
            Assert.That(ReadUInt32(request, 0), Is.EqualTo(74));
            Assert.That(ReadUInt32(request, 5), Is.EqualTo(40));
            Assert.That(ReadUInt64(request, 9), Is.EqualTo(7));
            Assert.That(ReadUInt64(request, 17), Is.EqualTo(11));
            Assert.That(ReadUInt64(request, 25), Is.EqualTo(13));
            Assert.That(ReadUInt16(request, 33), Is.EqualTo(43));

            byte[] applied = BattleSessionResumeProtocolCodec.EncodeSnapshotApplied(99);
            Assert.That(applied.Length, Is.EqualTo(17));
            Assert.That(ReadUInt32(applied, 0), Is.EqualTo(13));
            Assert.That(ReadUInt32(applied, 5), Is.EqualTo(41));
            Assert.That(ReadUInt64(applied, 9), Is.EqualTo(99));
        }

        [Test]
        public void FullSnapshotDecodesEveryAuthoritativeField()
        {
            byte[] frame = SnapshotFrame(
                requestId: 7,
                snapshotId: 8,
                roomId: 11,
                battleId: 13,
                playerSessionId: 17,
                generation: 19,
                phase: BattleResumePhase.Loot,
                remainingMillis: 2300,
                serverTick: 41,
                players: new[]
                {
                    new BattleResumePlayerState(17, 120, -40, true, 80, 100, true),
                    new BattleResumePlayerState(23, 900, 21, false, 0, 0, false)
                },
                monster: new BattleResumeMonsterState(29, 250, 350, 120, 600, BattleResumeMonsterState.Alive),
                drops: new[]
                {
                    new BattleResumeDropState(31, 37, 2, 10, 20, 1, 17)
                },
                score: 1234,
                result: null);

            BattleResumeSnapshot snapshot =
                BattleSessionResumeProtocolCodec.DecodeServerFrame(frame);

            Assert.That(snapshot.RequestId, Is.EqualTo(7));
            Assert.That(snapshot.SnapshotId, Is.EqualTo(8));
            Assert.That(snapshot.RoomId, Is.EqualTo(11));
            Assert.That(snapshot.BattleInstanceId, Is.EqualTo(13));
            Assert.That(snapshot.PlayerSessionId, Is.EqualTo(17));
            Assert.That(snapshot.SessionGeneration, Is.EqualTo(19));
            Assert.That(snapshot.Phase, Is.EqualTo(BattleResumePhase.Loot));
            Assert.That(snapshot.RemainingMilliseconds, Is.EqualTo(2300));
            Assert.That(snapshot.ServerTick, Is.EqualTo(41));
            Assert.That(snapshot.Players, Has.Count.EqualTo(2));
            Assert.That(snapshot.Players[0].HitPoints, Is.EqualTo(80));
            Assert.That(snapshot.Players[0].IsAlive, Is.True);
            Assert.That(snapshot.Players[1].HealthKnown, Is.False);
            Assert.That(snapshot.Monster.MonsterId, Is.EqualTo(29));
            Assert.That(snapshot.Monster.PositionXMillimeters, Is.EqualTo(250));
            Assert.That(snapshot.Drops[0].OwnerSessionId, Is.EqualTo(17));
            Assert.That(snapshot.Score, Is.EqualTo(1234));
            Assert.That(snapshot.HasResult, Is.False);
        }

        [Test]
        public void ResultSnapshotIsAcceptedAndMalformedFramesAreRejected()
        {
            var entries = new[]
            {
                new FinalResultEntry(17, "neo", FinalResultExitStatus.TerminalPresent, 90, 1, true),
                new FinalResultEntry(23, "trinity", FinalResultExitStatus.TerminalExited, 70, 2, false)
            };
            byte[] frame = SnapshotFrame(
                7, 8, 11, 13, 17, 19, BattleResumePhase.Result, 0, 99,
                new[]
                {
                    new BattleResumePlayerState(17, 0, 0, true, 50, 100, true),
                    new BattleResumePlayerState(23, 1, 1, true, 40, 100, true)
                },
                null, Array.Empty<BattleResumeDropState>(), 160,
                new BattleResumeResultState(FinalResultOutcome.MonsterDefeated, entries));

            BattleResumeSnapshot snapshot =
                BattleSessionResumeProtocolCodec.DecodeServerFrame(frame);
            Assert.That(snapshot.HasResult, Is.True);
            Assert.That(snapshot.Result.Entries[1].Nickname, Is.EqualTo("trinity"));
            Assert.That(snapshot.Result.Outcome, Is.EqualTo(FinalResultOutcome.MonsterDefeated));

            Assert.Throws<BattleSessionResumeProtocolException>(() =>
                BattleSessionResumeProtocolCodec.DecodeServerFrame(Truncate(frame)));
            byte[] wrongVersion = (byte[])frame.Clone();
            wrongVersion[4] = 2;
            Assert.Throws<BattleSessionResumeProtocolException>(() =>
                BattleSessionResumeProtocolCodec.DecodeServerFrame(wrongVersion));
        }

        [Test]
        public async Task TcpPumpRoutesSnapshotThroughTheSingleReceiveOwner()
        {
            var sink = new RecordingSnapshotSink();
            var router = new TypedServerEventRouter(new ImmediateDispatcher());
            using (router.Subscribe((IBattleResumeInboundMessageSink)sink))
            {
                await new TcpInboundPump(
                        new MemoryStream(SnapshotFrame(
                            7, 8, 11, 13, 17, 19, BattleResumePhase.Combat,
                            2000, 99,
                            new[]
                            {
                                new BattleResumePlayerState(17, 1, 2, true, 3, 4, true),
                                new BattleResumePlayerState(23, 2, 3, true, 3, 4, true)
                            },
                            null, Array.Empty<BattleResumeDropState>(), 0, null)),
                        router)
                    .RunAsync(CancellationToken.None);
            }

            Assert.That(sink.Last, Is.Not.Null);
            Assert.That(sink.Last.SnapshotId, Is.EqualTo(8));
        }

        private static byte[] SnapshotFrame(
            ulong requestId,
            ulong snapshotId,
            ulong roomId,
            ulong battleId,
            ulong playerSessionId,
            ulong generation,
            BattleResumePhase phase,
            uint remainingMillis,
            uint serverTick,
            IReadOnlyList<BattleResumePlayerState> players,
            BattleResumeMonsterState monster,
            IReadOnlyList<BattleResumeDropState> drops,
            ulong score,
            BattleResumeResultState result)
        {
            using (var stream = new MemoryStream())
            {
                stream.Write(new byte[4], 0, 4);
                stream.WriteByte(1);
                WriteUInt32(stream, 39);
                WriteUInt64(stream, requestId);
                WriteUInt64(stream, snapshotId);
                WriteUInt64(stream, roomId);
                WriteUInt64(stream, battleId);
                WriteUInt64(stream, playerSessionId);
                WriteUInt64(stream, generation);
                stream.WriteByte((byte)phase);
                WriteUInt32(stream, remainingMillis);
                WriteUInt32(stream, serverTick);
                WriteUInt16(stream, players.Count);
                foreach (BattleResumePlayerState player in players)
                {
                    WriteUInt64(stream, player.SessionId);
                    WriteInt32(stream, player.PositionXMillimeters);
                    WriteInt32(stream, player.PositionYMillimeters);
                    stream.WriteByte(player.HealthKnown ? (byte)1 : (byte)0);
                    WriteUInt32(stream, player.HitPoints);
                    WriteUInt32(stream, player.MaximumHitPoints);
                    stream.WriteByte(player.IsAlive ? (byte)1 : (byte)0);
                }
                stream.WriteByte(monster == null ? (byte)0 : (byte)1);
                if (monster != null)
                {
                    WriteUInt64(stream, monster.MonsterId);
                    WriteInt32(stream, monster.PositionXMillimeters);
                    WriteInt32(stream, monster.PositionYMillimeters);
                    WriteUInt32(stream, monster.HitPoints);
                    WriteUInt32(stream, monster.MaximumHitPoints);
                    stream.WriteByte(monster.State);
                }
                WriteUInt16(stream, drops.Count);
                foreach (BattleResumeDropState drop in drops)
                {
                    WriteUInt64(stream, drop.DropId);
                    WriteUInt64(stream, drop.ItemId);
                    WriteUInt64(stream, drop.Quantity);
                    WriteInt32(stream, drop.PositionXMillimeters);
                    WriteInt32(stream, drop.PositionYMillimeters);
                    stream.WriteByte(drop.State);
                    WriteUInt64(stream, drop.OwnerSessionId);
                }
                WriteUInt64(stream, score);
                stream.WriteByte(result == null ? (byte)0 : (byte)1);
                if (result != null)
                {
                    stream.WriteByte((byte)result.Outcome);
                    WriteUInt16(stream, result.Entries.Count);
                    foreach (FinalResultEntry entry in result.Entries)
                    {
                        WriteUInt64(stream, entry.SessionId);
                        byte[] nickname = System.Text.Encoding.UTF8.GetBytes(entry.Nickname);
                        WriteUInt16(stream, nickname.Length);
                        stream.Write(nickname, 0, nickname.Length);
                        stream.WriteByte((byte)entry.ExitStatus);
                        WriteUInt64(stream, entry.FinalAssetValue);
                        WriteUInt32(stream, entry.Rank ?? 0);
                        stream.WriteByte(entry.IsTop ? (byte)1 : (byte)0);
                    }
                }

                byte[] frame = stream.ToArray();
                WriteUInt32(frame, 0, (uint)(frame.Length - 4));
                return frame;
            }
        }

        private static byte[] Truncate(byte[] frame)
        {
            var result = new byte[frame.Length - 1];
            Buffer.BlockCopy(frame, 0, result, 0, result.Length);
            WriteUInt32(result, 0, (uint)(result.Length - 4));
            return result;
        }

        private static void WriteUInt16(Stream stream, int value)
        {
            stream.WriteByte((byte)(value >> 8));
            stream.WriteByte((byte)value);
        }

        private static void WriteUInt32(Stream stream, uint value)
        {
            stream.WriteByte((byte)(value >> 24));
            stream.WriteByte((byte)(value >> 16));
            stream.WriteByte((byte)(value >> 8));
            stream.WriteByte((byte)value);
        }

        private static void WriteUInt64(Stream stream, ulong value)
        {
            WriteUInt32(stream, (uint)(value >> 32));
            WriteUInt32(stream, (uint)value);
        }

        private static void WriteInt32(Stream stream, int value)
        {
            WriteUInt32(stream, unchecked((uint)value));
        }

        private static void WriteUInt32(byte[] bytes, int offset, uint value)
        {
            bytes[offset] = (byte)(value >> 24);
            bytes[offset + 1] = (byte)(value >> 16);
            bytes[offset + 2] = (byte)(value >> 8);
            bytes[offset + 3] = (byte)value;
        }

        private static ushort ReadUInt16(byte[] bytes, int offset)
        {
            return (ushort)((bytes[offset] << 8) | bytes[offset + 1]);
        }

        private static uint ReadUInt32(byte[] bytes, int offset)
        {
            return ((uint)bytes[offset] << 24) |
                   ((uint)bytes[offset + 1] << 16) |
                   ((uint)bytes[offset + 2] << 8) |
                   bytes[offset + 3];
        }

        private static ulong ReadUInt64(byte[] bytes, int offset)
        {
            ulong value = 0;
            for (int index = 0; index < 8; index++)
            {
                value = (value << 8) | bytes[offset + index];
            }
            return value;
        }

        private sealed class RecordingSnapshotSink : IBattleResumeInboundMessageSink
        {
            public BattleResumeSnapshot Last { get; private set; }

            public void OnMessage(BattleResumeSnapshot message)
            {
                Last = message;
            }
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
