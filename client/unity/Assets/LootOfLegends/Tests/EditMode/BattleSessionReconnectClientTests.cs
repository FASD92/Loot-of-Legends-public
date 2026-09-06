using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;
using NUnit.Framework;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class BattleSessionReconnectClientTests
    {
        private const string Credential =
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

        [Test]
        public async Task ResumeLocksInputSendsFreshCredentialAndAppliesSnapshotBeforeAck()
        {
            var clock = new FakeClock();
            var sender = new RecordingSender();
            var applied = new List<BattleResumeSnapshot>();
            var client = new BattleSessionReconnectClient(
                sender,
                _ => Task.FromResult(Credential),
                clock,
                snapshot =>
                {
                    applied.Add(snapshot);
                    return true;
                });

            Task<BattleSessionReconnectResult> pending = client.ResumeAsync(
                11,
                13,
                _ => Task.FromResult<ITcpCommandSender>(sender),
                CancellationToken.None);
            Assert.That(client.IsInputLocked, Is.True);
            await sender.FrameSent;
            client.OnMessage(new WelcomeSession(1, 11, 13, 0, "neo"));
            client.OnMessage(Snapshot(1, 11, 13, BattleResumePhase.Combat));

            BattleSessionReconnectResult result = await pending;
            Assert.That(result.Status, Is.EqualTo(BattleSessionReconnectStatus.Resumed));
            Assert.That(result.RequiresRudpRebind, Is.True);
            Assert.That(client.IsInputLocked, Is.False);
            Assert.That(applied, Has.Count.EqualTo(1));
            Assert.That(sender.Frames, Has.Count.EqualTo(2));
            Assert.That(ReadUInt32(sender.Frames[0], 5), Is.EqualTo(40));
            Assert.That(ReadUInt32(sender.Frames[1], 5), Is.EqualTo(41));
            Assert.That(ReadUInt64(sender.Frames[1], 9), Is.EqualTo(8));
        }

        [Test]
        public async Task ResultResumeCompletesWithoutRudpRebindRequirement()
        {
            var sender = new RecordingSender();
            var applied = new List<BattleResumeSnapshot>();
            var client = new BattleSessionReconnectClient(
                sender,
                _ => Task.FromResult(Credential),
                new FakeClock(),
                snapshot =>
                {
                    applied.Add(snapshot);
                    return true;
                });

            Task<BattleSessionReconnectResult> pending = client.ResumeAsync(
                11,
                13,
                _ => Task.FromResult<ITcpCommandSender>(sender),
                CancellationToken.None);
            await sender.FrameSent;
            client.OnMessage(new WelcomeSession(1, 11, 13, 0, "neo"));
            client.OnMessage(ResultSnapshot(1, 11, 13));

            BattleSessionReconnectResult result = await pending;
            Assert.That(result.Status, Is.EqualTo(BattleSessionReconnectStatus.Resumed));
            Assert.That(result.Snapshot.Phase, Is.EqualTo(BattleResumePhase.Result));
            Assert.That(result.RequiresRudpRebind, Is.False);
            Assert.That(client.IsInputLocked, Is.False);
            Assert.That(applied, Has.Count.EqualTo(1));
            Assert.That(sender.Frames, Has.Count.EqualTo(2));
            Assert.That(ReadUInt32(sender.Frames[1], 5), Is.EqualTo(41));
        }

        [Test]
        public async Task TwoConcurrentResumeRequestsHaveOneOwnerAndBackoffExpires()
        {
            var clock = new FakeClock();
            var firstAttempt = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            var releaseFirstAttempt = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            int credentials = 0;
            var sender = new RecordingSender();
            var client = new BattleSessionReconnectClient(
                sender,
                _ =>
                {
                    credentials++;
                    return Task.FromResult(Credential);
                },
                clock,
                _ => true);

            Task<BattleSessionReconnectResult> first = client.ResumeAsync(
                11, 13, async _ =>
                {
                    firstAttempt.TrySetResult(true);
                    await releaseFirstAttempt.Task;
                    throw new BattleResumeUnavailableException();
                }, CancellationToken.None);
            Task<BattleSessionReconnectResult> second = client.ResumeAsync(
                11, 13, _ => Task.FromResult<ITcpCommandSender>(sender),
                CancellationToken.None);

            BattleSessionReconnectResult duplicate = await second;
            Assert.That(duplicate.Status,
                Is.EqualTo(BattleSessionReconnectStatus.AlreadyRunning));
            await firstAttempt.Task;
            clock.Advance(30000);
            releaseFirstAttempt.TrySetResult(true);
            BattleSessionReconnectResult expired = await first;
            Assert.That(expired.Status,
                Is.EqualTo(BattleSessionReconnectStatus.GraceExpired));
            Assert.That(credentials, Is.GreaterThan(0));
            Assert.That(client.IsInputLocked, Is.False);
        }

        [Test]
        public async Task AuthenticationFailureStopsWithoutRetryLoop()
        {
            var clock = new FakeClock();
            int attempts = 0;
            var client = new BattleSessionReconnectClient(
                new RecordingSender(),
                _ => Task.FromResult(Credential),
                clock,
                _ => true);

            BattleSessionReconnectResult result = await client.ResumeAsync(
                11,
                13,
                _ =>
                {
                    attempts++;
                    return Task.FromException<ITcpCommandSender>(
                        new BattleResumeRejectedException(
                            AuthenticationRejectedReason.ResumeUnavailable));
                },
                CancellationToken.None);

            Assert.That(result.Status,
                Is.EqualTo(BattleSessionReconnectStatus.AuthenticationFailed));
            Assert.That(attempts, Is.EqualTo(1));
            Assert.That(client.IsInputLocked, Is.False);
        }

        [Test]
        public async Task RecoveryWelcomeRotatesGenerationBeforeSnapshotValidation()
        {
            var sender = new RecordingSender();
            var welcomes = new List<WelcomeSession>();
            var client = new BattleSessionReconnectClient(
                sender,
                _ => Task.FromResult(Credential),
                new FakeClock(),
                _ => true,
                null,
                (requestId, welcome) =>
                {
                    Assert.That(requestId, Is.EqualTo(welcome.RequestId));
                    welcomes.Add(welcome);
                    return true;
                });

            Task<BattleSessionReconnectResult> pending = client.ResumeAsync(
                11,
                13,
                _ => Task.FromResult<ITcpCommandSender>(sender),
                CancellationToken.None,
                new BattleRecoveryResumeProof(7, 9, 11, 13));
            await sender.FrameSent;
            client.OnMessage(new WelcomeSession(1, 11, 14, 0, "neo"));
            client.OnMessage(Snapshot(1, 7, 9, 11, 14, BattleResumePhase.Combat));

            BattleSessionReconnectResult result = await pending;
            Assert.That(result.Status, Is.EqualTo(BattleSessionReconnectStatus.Resumed));
            Assert.That(result.Snapshot.SessionGeneration, Is.EqualTo(14));
            Assert.That(client.CurrentSessionGeneration, Is.EqualTo(14));
            Assert.That(welcomes, Has.Count.EqualTo(1));
        }

        [Test]
        public async Task ResumeUnavailableRetriesOriginalRecoveryProofOnceAfterCurrentGeneration()
        {
            var clock = new FakeClock();
            var first = new ScriptedSender();
            var second = new ScriptedSender();
            var senders = new Queue<ITcpCommandSender>(new[] { first, second });
            int credentials = 0;
            var client = new BattleSessionReconnectClient(
                first,
                _ =>
                {
                    credentials++;
                    return Task.FromResult(Credential);
                },
                clock,
                _ => true,
                null,
                (requestId, welcome) => true);

            first.OnSend = _ =>
            {
                client.OnMessage(new WelcomeSession(1, 11, 14, 0, "neo"));
                client.OnMessage(new AuthenticationRejectedSession(
                    1,
                    AuthenticationRejectedReason.ResumeUnavailable));
            };
            second.OnSend = _ =>
            {
                client.OnMessage(new WelcomeSession(2, 11, 14, 0, "neo"));
                client.OnMessage(Snapshot(2, 7, 9, 11, 14, BattleResumePhase.Combat));
            };

            Task<BattleSessionReconnectResult> pending = client.ResumeAsync(
                11,
                14,
                _ => Task.FromResult(senders.Dequeue()),
                CancellationToken.None,
                new BattleRecoveryResumeProof(7, 9, 11, 13));

            BattleSessionReconnectResult result = await pending;
            Assert.That(result.Status, Is.EqualTo(BattleSessionReconnectStatus.Resumed));
            Assert.That(credentials, Is.EqualTo(2));
            Assert.That(first.Frames, Has.Count.EqualTo(1));
            Assert.That(second.Frames, Has.Count.EqualTo(2));
            Assert.That(ReadUInt64(first.Frames[0], 25), Is.EqualTo(14));
            Assert.That(ReadUInt64(second.Frames[0], 25), Is.EqualTo(13));
        }

        [Test]
        public async Task MismatchedWelcomeDoesNotRotateGenerationOrSatisfyWaiter()
        {
            var sender = new RecordingSender();
            ulong appliedGeneration = 0;
            var client = new BattleSessionReconnectClient(
                sender,
                _ => Task.FromResult(Credential),
                new FakeClock(),
                _ => true,
                null,
                (requestId, welcome) =>
                {
                    appliedGeneration = welcome.SessionGeneration;
                    return true;
                });

            Task<BattleSessionReconnectResult> pending = client.ResumeAsync(
                11,
                13,
                _ => Task.FromResult<ITcpCommandSender>(sender),
                CancellationToken.None);
            await sender.FrameSent;
            client.OnMessage(new WelcomeSession(99, 11, 99, 0, "stale"));
            Assert.That(client.CurrentSessionGeneration, Is.EqualTo(13));
            client.OnMessage(new WelcomeSession(1, 11, 14, 0, "neo"));
            client.OnMessage(Snapshot(1, 7, 9, 11, 14, BattleResumePhase.Combat));

            Assert.That((await pending).Status,
                Is.EqualTo(BattleSessionReconnectStatus.Resumed));
            Assert.That(appliedGeneration, Is.EqualTo(14));
        }

        [Test]
        public async Task MatchingRequestWithWrongSessionDoesNotRotateGeneration()
        {
            var sender = new RecordingSender();
            var client = new BattleSessionReconnectClient(
                sender,
                _ => Task.FromResult(Credential),
                new FakeClock(),
                _ => true);

            Task<BattleSessionReconnectResult> pending = client.ResumeAsync(
                11,
                13,
                _ => Task.FromResult<ITcpCommandSender>(sender),
                CancellationToken.None);
            await sender.FrameSent;
            client.OnMessage(new WelcomeSession(1, 99, 14, 0, "stale"));

            BattleSessionReconnectResult result = await pending;
            Assert.That(result.Status, Is.EqualTo(BattleSessionReconnectStatus.Failed));
            Assert.That(client.CurrentSessionGeneration, Is.EqualTo(13));
        }

        private static BattleResumeSnapshot Snapshot(
            ulong requestId,
            ulong roomId,
            ulong battleId,
            BattleResumePhase phase)
        {
            return Snapshot(requestId, roomId, battleId, 11, 13, phase);
        }

        private static BattleResumeSnapshot Snapshot(
            ulong requestId,
            ulong roomId,
            ulong battleId,
            ulong playerSessionId,
            ulong sessionGeneration,
            BattleResumePhase phase)
        {
            return new BattleResumeSnapshot(
                requestId,
                8,
                roomId,
                battleId,
                playerSessionId,
                sessionGeneration,
                phase,
                1000,
                4,
                new[]
                {
                    new BattleResumePlayerState(11, 1, 2, true, 90, 100, true),
                    new BattleResumePlayerState(23, 2, 3, true, 90, 100, true)
                },
                null,
                Array.Empty<BattleResumeDropState>(),
                9,
                null);
        }

        private static BattleResumeSnapshot ResultSnapshot(
            ulong requestId,
            ulong roomId,
            ulong battleId)
        {
            return new BattleResumeSnapshot(
                requestId,
                8,
                roomId,
                battleId,
                11,
                13,
                BattleResumePhase.Result,
                0,
                4,
                new[]
                {
                    new BattleResumePlayerState(11, 1, 2, true, 90, 100, true),
                    new BattleResumePlayerState(23, 2, 3, true, 90, 100, true)
                },
                null,
                Array.Empty<BattleResumeDropState>(),
                9,
                new BattleResumeResultState(
                    FinalResultOutcome.MonsterDefeated,
                    new[]
                    {
                        new FinalResultEntry(
                            11,
                            "neo",
                            FinalResultExitStatus.TerminalPresent,
                            90,
                            1,
                            true),
                        new FinalResultEntry(
                            23,
                            "trinity",
                            FinalResultExitStatus.TerminalExited,
                            70,
                            2,
                            false)
                    }));
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

        private sealed class RecordingSender : ITcpCommandSender
        {
            private readonly TaskCompletionSource<bool> frameSent =
                new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously);

            public List<byte[]> Frames { get; } = new List<byte[]>();
            public Task FrameSent => frameSent.Task;

            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                Frames.Add(frame);
                frameSent.TrySetResult(true);
                return Task.CompletedTask;
            }
        }

        private sealed class ScriptedSender : ITcpCommandSender
        {
            public List<byte[]> Frames { get; } = new List<byte[]>();
            public Action<byte[]> OnSend { get; set; }

            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                Frames.Add(frame);
                OnSend?.Invoke(frame);
                return Task.CompletedTask;
            }
        }

        private sealed class FakeClock : IMonotonicReconnectClock
        {
            public long NowMilliseconds { get; private set; }

            public Task DelayAsync(int milliseconds, CancellationToken cancellationToken)
            {
                NowMilliseconds += milliseconds;
                return Task.CompletedTask;
            }

            public void Advance(long milliseconds)
            {
                NowMilliseconds += milliseconds;
            }
        }
    }
}
