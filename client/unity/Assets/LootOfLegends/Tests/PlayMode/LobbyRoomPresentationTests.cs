using System;
using System.Collections;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Presentation.Lobby;
using LootOfLegends.Presentation.Room;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class LobbyRoomPresentationTests
    {
        [UnityTest]
        public IEnumerator LobbyEntryCreateJoinAndFullResultNeverPredictRoomState()
        {
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            var commands = new LobbyRoomCommandCoordinator(sender, correlator);
            var view = new RecordingLobbyView();
            using (var presenter = new LobbyPresenter(readModel, commands, view))
            {
                presenter.Begin();
                router.OnMessage(new LobbyEntrySnapshot(
                    1,
                    2,
                    "neo",
                    new[] { new RoomSummary(7, "room", 1, 2) }));

                Assert.That(view.Last.Nickname, Is.EqualTo("neo"));
                Assert.That(view.Last.Rooms, Has.Count.EqualTo(1));

                Task create = presenter.CreateAsync("room", 2, CancellationToken.None);
                yield return null;
                Assert.That(sender.Frames[0], Is.EqualTo(Hex(
                    "000000130100000007000000000000000104726f6f6d02")));
                router.OnMessage(new RoomCommandResponse(1, 0));
                yield return new WaitUntil(() => create.IsCompleted);
                Assert.That(view.LastResult, Is.EqualTo(RoomCommandResult.Ok));
                Assert.That(readModel.Room, Is.Null,
                    "OK response must not invent a Room before projection");

                Task join = presenter.JoinAsync(7, CancellationToken.None);
                yield return null;
                Assert.That(sender.Frames[1], Is.EqualTo(Hex(
                    "00000015010000000800000000000000020000000000000007")));
                router.OnMessage(new RoomCommandResponse(2, 5));
                yield return new WaitUntil(() => join.IsCompleted);
                Assert.That(view.LastResult, Is.EqualTo(RoomCommandResult.RoomFull));
                Assert.That(readModel.Room, Is.Null);
                Assert.That(view.Last.Rooms[0].MemberCount, Is.EqualTo(1));
            }
        }

        [UnityTest]
        public IEnumerator RoomControlsWaitForServerProjection()
        {
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            var commands = new LobbyRoomCommandCoordinator(sender, correlator);
            var hostStart = new RecordingHostStart();
            var view = new RecordingRoomView();
            using (var presenter = new RoomPresenter(
                readModel,
                commands,
                hostStart,
                view))
            {
                presenter.Begin();
                router.OnMessage(new LobbyEntrySnapshot(
                    1, 2, "neo", Array.Empty<RoomSummary>()));
                router.OnMessage(Room(firstReady: false, secondReady: true));

                Assert.That(view.Last.IsLocalHost, Is.True);
                Assert.That(view.Last.CanStart, Is.False);
                Assert.That(view.Last.Members[0].Ready, Is.False);

                Task ready = presenter.SetReadyAsync(true, CancellationToken.None);
                yield return null;
                Assert.That(sender.Frames[0], Is.EqualTo(Hex(
                    "0000000e010000000a000000000000000101")));
                router.OnMessage(new RoomCommandResponse(1, 0));
                yield return new WaitUntil(() => ready.IsCompleted);
                Assert.That(view.Last.Members[0].Ready, Is.False,
                    "command response must not predict readiness");

                router.OnMessage(Room(firstReady: true, secondReady: true));
                Assert.That(view.Last.Members[0].Ready, Is.True);
                Assert.That(view.Last.CanStart, Is.True);

                Task start = presenter.HostStartAsync(CancellationToken.None);
                yield return new WaitUntil(() => start.IsCompleted);
                Assert.That(hostStart.Calls, Is.EqualTo(1));

                Task kick = presenter.KickAsync(2, 3, CancellationToken.None);
                yield return null;
                Assert.That(sender.Frames[1], Is.EqualTo(Hex(
                    "0000001d010000000b000000000000000200000000000000020000000000000003")));
                router.OnMessage(new RoomCommandResponse(2, 0));
                yield return new WaitUntil(() => kick.IsCompleted);
                Assert.That(view.Last.Members, Has.Count.EqualTo(2));

                Task leave = presenter.LeaveAsync(CancellationToken.None);
                yield return null;
                Assert.That(sender.Frames[2], Is.EqualTo(Hex(
                    "0000000d01000000090000000000000003")));
                router.OnMessage(new RoomCommandResponse(3, 0));
                yield return new WaitUntil(() => leave.IsCompleted);
                Assert.That(readModel.Room, Is.Not.Null,
                    "leave response must wait for LobbyEntry projection");

                router.OnMessage(new LobbyEntrySnapshot(
                    1, 2, "neo", Array.Empty<RoomSummary>()));
                Assert.That(readModel.Room, Is.Null);
            }
        }

        [UnityTest]
        public IEnumerator DisposedPresenterIgnoresLateCommandCompletion()
        {
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            var view = new RecordingLobbyView();
            var presenter = new LobbyPresenter(
                readModel,
                new LobbyRoomCommandCoordinator(sender, correlator),
                view);
            presenter.Begin();

            Task join = presenter.JoinAsync(7, CancellationToken.None);
            yield return null;
            presenter.Dispose();
            router.OnMessage(new RoomCommandResponse(1, 0));
            yield return new WaitUntil(() => join.IsCompleted);

            Assert.That(view.ResultCalls, Is.Zero);
        }

        [UnityTest]
        public IEnumerator VoluntaryBattleLeaveWaitsForServerLobbyProjection()
        {
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            router.OnMessage(new LobbyEntrySnapshot(
                1, 2, "neo", Array.Empty<RoomSummary>()));
            router.OnMessage(Room(firstReady: false, secondReady: false));
            var view = new RecordingArenaExitView();
            var navigation = new RecordingLobbyNavigation();
            using (var presenter = new ArenaExitPresenter(
                readModel,
                new LobbyRoomCommandCoordinator(sender, correlator),
                view,
                navigation))
            {
                presenter.Begin();
                Task leave = presenter.LeaveAsync(CancellationToken.None);
                yield return null;
                router.OnMessage(new RoomCommandResponse(1, 0));
                yield return new WaitUntil(() => leave.IsCompleted);

                Assert.That(navigation.Calls, Is.Zero,
                    "command OK must not predict Lobby entry");
                router.OnMessage(new LobbyRoomListUpdate(Array.Empty<RoomSummary>()));
                yield return null;

                Assert.That(navigation.Calls, Is.EqualTo(1));
                Assert.That(view.Copy, Is.EqualTo("전투에서 나와 로비로 돌아갑니다."));
                Assert.That(readModel.CurrentRoom, Is.Null);
            }
        }

        [UnityTest]
        public IEnumerator ConfirmedDisconnectIsShownOnlyByRoomProjection()
        {
            var readModel = new LobbyRoomReadModel();
            readModel.Apply(new LobbyEntrySnapshot(
                1, 2, "neo", Array.Empty<RoomSummary>()));
            var view = new RecordingRoomView();
            using (var presenter = new RoomPresenter(
                readModel,
                new LobbyRoomCommandCoordinator(
                    new RecordingSender(),
                    new RoomCommandCorrelator()),
                new RecordingHostStart(),
                view))
            {
                presenter.Begin();
                readModel.Apply(Room(firstReady: false, secondReady: false));
                Assert.That(view.Last.Members, Has.Count.EqualTo(2));

                readModel.Apply(new RoomDetailProjection(
                    7,
                    "room",
                    2,
                    1,
                    2,
                    new[] { new RoomMember(1, 2, "neo", false) }));
                yield return null;

                Assert.That(view.Last.Members, Has.Count.EqualTo(1));
                Assert.That(view.Last.Members[0].Nickname, Is.EqualTo("neo"));
            }
        }

        private static RoomDetailProjection Room(bool firstReady, bool secondReady)
        {
            return new RoomDetailProjection(
                7,
                "room",
                2,
                1,
                2,
                new[]
                {
                    new RoomMember(1, 2, "neo", firstReady),
                    new RoomMember(2, 3, "trinity", secondReady)
                });
        }

        private static byte[] Hex(string hex)
        {
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

        private sealed class RecordingHostStart : IRoomHostStartAction
        {
            public int Calls { get; private set; }

            public Task<RoomCommandResult> StartAsync(CancellationToken cancellationToken)
            {
                Calls++;
                return Task.FromResult(RoomCommandResult.Ok);
            }
        }

        private sealed class RecordingLobbyView : ILobbyView
        {
            public LobbyPresentationSnapshot Last { get; private set; }
            public RoomCommandResult LastResult { get; private set; }
            public int ResultCalls { get; private set; }

            public void Render(LobbyPresentationSnapshot snapshot)
            {
                Last = snapshot;
            }

            public void ShowCommandResult(RoomCommandResult result)
            {
                LastResult = result;
                ResultCalls++;
            }
        }

        private sealed class RecordingRoomView : IRoomView
        {
            public RoomPresentationSnapshot Last { get; private set; }
            public RoomCommandResult LastResult { get; private set; }

            public void Render(RoomPresentationSnapshot snapshot)
            {
                Last = snapshot;
            }

            public void ShowCommandResult(RoomCommandResult result)
            {
                LastResult = result;
            }
        }

        private sealed class RecordingArenaExitView : IArenaExitView
        {
            public string Copy { get; private set; }

            public void ShowExitStatus(string copy)
            {
                Copy = copy;
            }
        }

        private sealed class RecordingLobbyNavigation : ILobbyNavigation
        {
            public int Calls { get; private set; }

            public void ReturnToLobby()
            {
                Calls++;
            }
        }
    }
}
