using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
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
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using UnityEngine.UI;

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
        public IEnumerator ProductLobbyScreenDelegatesCreateAndWaitsForProjection()
        {
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            var commands = new LobbyRoomCommandCoordinator(sender, correlator);
            var owner = new GameObject("LobbyScreenViewTest");
            var view = owner.AddComponent<LobbyScreenView>();
            try
            {
                using (var presenter = new LobbyPresenter(readModel, commands, view))
                {
                    view.Bind(presenter, CancellationToken.None);
                    presenter.Begin();
                    router.OnMessage(new LobbyEntrySnapshot(
                        1, 2, "neo", Array.Empty<RoomSummary>()));

                    Task create = view.CreateAsync("room");
                    yield return null;
                    Assert.That(sender.Frames[0], Is.EqualTo(Hex(
                        "000000130100000007000000000000000104726f6f6d02")));
                    router.OnMessage(new RoomCommandResponse(1, 0));
                    yield return new WaitUntil(() => create.IsCompleted);

                    StringAssert.Contains("서버 상태 반영", view.StatusCopy);
                    Assert.That(view.Snapshot.Nickname, Is.EqualTo("neo"));
                    Assert.That(readModel.Room, Is.Null,
                        "screen must wait for the server Room projection");
                }
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(owner);
            }
        }

        [UnityTest]
        public IEnumerator ProductLobbyScreenRejectsInvalidTitleWithoutThrowing()
        {
            var sender = new RecordingSender();
            var commands = new LobbyRoomCommandCoordinator(
                sender,
                new RoomCommandCorrelator());
            var owner = new GameObject("LobbyScreenInvalidTitleTest");
            var view = owner.AddComponent<LobbyScreenView>();
            try
            {
                using (var presenter = new LobbyPresenter(
                           new LobbyRoomReadModel(),
                           commands,
                           view))
                {
                    view.Bind(presenter, CancellationToken.None);
                    presenter.Begin();
                    Task validation = null;

                    Assert.DoesNotThrow(() =>
                        validation = view.CreateAsync(new string('가', 17)));

                    Assert.That(validation.IsCompleted, Is.True);
                    Assert.That(sender.Frames, Is.Empty);
                    StringAssert.Contains("방 제목", view.StatusCopy);
                }
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(owner);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator LobbyLoadedProjectionReplacesOnlyInitialWaitingCopy()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            LobbyScreenView view = UnityEngine.Object.FindFirstObjectByType<LobbyScreenView>();
            Text status = view.GetComponentsInChildren<Text>(true)
                .Single(label => label.name == "StatusLabel");
            Text nickname = view.GetComponentsInChildren<Text>(true)
                .Single(label => label.name == "NicknameLabel");
            try
            {
                Assert.That(view.StatusCopy,
                    Is.EqualTo("서버 로비 정보를 기다리는 중입니다."));

                view.Render(new LobbyPresentationSnapshot(
                    0,
                    "portfolio-player",
                    Array.Empty<LobbyRoomSummaryView>()));
                Assert.That(view.StatusCopy,
                    Is.EqualTo("서버 로비 정보를 기다리는 중입니다."));

                view.Render(LobbySnapshot());
                Assert.That(view.StatusCopy,
                    Is.EqualTo("입장할 방을 선택하거나 새 방을 만드세요."));
                Assert.That(status.text,
                    Is.EqualTo("입장할 방을 선택하거나 새 방을 만드세요."));
                Assert.That(nickname.text, Is.EqualTo("portfolio-player"));
                Assert.That(view.Snapshot.Nickname, Is.EqualTo("portfolio-player"));
                Assert.That(view.Snapshot.Rooms.Select(room => room.Title), Is.EqualTo(new[]
                {
                    "Treasure Hunters", "Full Expedition"
                }));
                Assert.That(view.GetComponentsInChildren<Button>(true)
                    .Select(button => button.name), Does.Contain("RoomButton-7"));
                Assert.That(view.GetComponentsInChildren<Button>(true)
                    .Select(button => button.name), Does.Contain("RoomButton-8"));
                Assert.That(view.GetComponentsInChildren<InputField>(true)
                    .Single(input => input.name == "RoomTitleInput").interactable,
                    Is.False);
                Assert.That(view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "CreateButton").interactable,
                    Is.False);
                Assert.That(view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "RoomButton-7").interactable,
                    Is.False);
                Assert.That(view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "RoomButton-8").interactable,
                    Is.False);

                view.ShowCommandResult(RoomCommandResult.RoomFull);
                view.Render(LobbySnapshot());
                StringAssert.Contains("RoomFull", view.StatusCopy);
            }
            finally
            {
                SceneManager.LoadScene("LoginScene");
            }
        }

        [UnityTest]
        public IEnumerator LobbyProjectionNeverOverwritesPendingOrFailureCopy()
        {
            var owner = new GameObject("LobbyTransientStatusTest");
            var view = owner.AddComponent<LobbyScreenView>();
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var model = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, model);
            try
            {
                using (var presenter = new LobbyPresenter(
                           model,
                           new LobbyRoomCommandCoordinator(sender, correlator),
                           view))
                {
                    view.Bind(presenter, CancellationToken.None);
                    presenter.Begin();
                    router.OnMessage(new LobbyEntrySnapshot(
                        1,
                        2,
                        "portfolio-player",
                        new[]
                        {
                            new RoomSummary(7, "Treasure Hunters", 1, 2),
                            new RoomSummary(8, "Full Expedition", 2, 2)
                        }));
                    Assert.That(view.StatusCopy,
                        Is.EqualTo("입장할 방을 선택하거나 새 방을 만드세요."));

                    Task create = view.CreateAsync("Player Room");
                    view.Render(LobbySnapshot());
                    Assert.That(view.StatusCopy, Is.EqualTo("방 생성 요청 중입니다."));
                    router.OnMessage(new RoomCommandResponse(1, 0));
                    yield return new WaitUntil(() => create.IsCompleted);
                    yield return null;
                    Assert.That(view.StatusCopy,
                        Is.EqualTo("요청이 접수되었습니다. 서버 상태 반영을 기다립니다."));
                    view.Render(LobbySnapshot());
                    Assert.That(view.StatusCopy,
                        Is.EqualTo("요청이 접수되었습니다. 서버 상태 반영을 기다립니다."));

                    Task join = view.JoinAsync(7);
                    view.Render(LobbySnapshot());
                    Assert.That(view.StatusCopy, Is.EqualTo("방 참가 요청 중입니다."));
                    router.OnMessage(new RoomCommandResponse(2,
                        (ushort)RoomCommandResult.RoomFull));
                    yield return new WaitUntil(() => join.IsCompleted);
                    yield return null;
                    StringAssert.Contains("RoomFull", view.StatusCopy);
                    view.Render(LobbySnapshot());
                    StringAssert.Contains("RoomFull", view.StatusCopy);
                }

                using (var presenter = new LobbyPresenter(
                           new LobbyRoomReadModel(),
                           new LobbyRoomCommandCoordinator(
                               new ThrowingSender(), new RoomCommandCorrelator()),
                           view))
                {
                    view.Bind(presenter, CancellationToken.None);
                    presenter.Begin();
                    Task failed = view.CreateAsync("Player Room");
                    yield return new WaitUntil(() => failed.IsCompleted);
                    yield return null;
                    Assert.That(failed.IsFaulted, Is.True);
                    Assert.That(view.StatusCopy, Is.EqualTo("요청을 완료하지 못했습니다."));
                    view.Render(LobbySnapshot());
                    Assert.That(view.StatusCopy, Is.EqualTo("요청을 완료하지 못했습니다."));
                }
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(owner);
            }
        }

        [UnityTest]
        public IEnumerator LobbyRoomListScrollsFortyServerRooms()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            LobbyScreenView view =
                UnityEngine.Object.FindFirstObjectByType<LobbyScreenView>();
            LobbyRoomSummaryView[] rooms = Enumerable.Range(1, 40)
                .Select(index => new LobbyRoomSummaryView(
                    (ulong)index, "Room " + index, 1, 2))
                .ToArray();
            view.Render(new LobbyPresentationSnapshot(1, "portfolio-player", rooms));
            Canvas.ForceUpdateCanvases();
            yield return null;

            Button[] roomButtons = view.GetComponentsInChildren<Button>(true)
                .Where(button => button.name.StartsWith("RoomButton-"))
                .ToArray();
            ScrollRect scroll = roomButtons[0].GetComponentInParent<ScrollRect>();
            Assert.That(roomButtons, Has.Length.EqualTo(40));
            Assert.That(scroll, Is.Not.Null);
            Assert.That(scroll.content.rect.height,
                Is.GreaterThan(scroll.viewport.rect.height));
            Assert.That(scroll.vertical, Is.True);
            Assert.That(scroll.horizontal, Is.False);
        }

        [UnityTest]
        public IEnumerator LobbyCreateButtonSubmitsExistingPresenterIntent()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            LobbyScreenView view =
                UnityEngine.Object.FindFirstObjectByType<LobbyScreenView>();
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            using (var presenter = new LobbyPresenter(
                       readModel,
                       new LobbyRoomCommandCoordinator(sender, correlator),
                       view))
            {
                view.Bind(presenter, CancellationToken.None);
                presenter.Begin();
                router.OnMessage(new LobbyEntrySnapshot(
                    1,
                    2,
                    "neo",
                    new[]
                    {
                        new RoomSummary(7, "Treasure Hunters", 1, 2),
                        new RoomSummary(8, "Full Expedition", 2, 2)
                    }));

                Button available = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "RoomButton-7");
                Button full = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "RoomButton-8");
                Text availableLabel = available.GetComponentInChildren<Text>(true);
                Text fullLabel = full.GetComponentInChildren<Text>(true);
                InputField title = view.GetComponentsInChildren<InputField>(true)
                    .Single(input => input.name == "RoomTitleInput");
                InputField capacity = view.GetComponentsInChildren<InputField>(true)
                    .Single(input => input.name == "RoomCapacityInput");
                Button create = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "CreateButton");
                Assert.That(
                    availableLabel.text,
                    Is.EqualTo("Treasure Hunters   1/2"));
                Assert.That(availableLabel.color, Is.EqualTo(Color.white));
                Assert.That(available.interactable, Is.True);
                Assert.That(
                    fullLabel.text,
                    Is.EqualTo("Full Expedition   2/2   만석"));
                Assert.That(
                    fullLabel.color,
                    Is.EqualTo((Color)new Color32(0x5A, 0x35, 0x1B, 0xFF)));
                Assert.That(full.interactable, Is.False);

                Assert.That(title.interactable, Is.True);
                Assert.That(capacity.interactable, Is.True);
                Assert.That(capacity.contentType,
                    Is.EqualTo(InputField.ContentType.IntegerNumber));
                Assert.That(capacity.characterLimit, Is.EqualTo(2));
                Assert.That(capacity.text, Is.EqualTo("2"));
                Assert.That(create.interactable, Is.True);
                title.text = "room";
                capacity.text = "10";
                create.onClick.Invoke();
                yield return null;

                Assert.That(sender.Frames, Has.Count.EqualTo(1));
                Assert.That(sender.Frames[0], Is.EqualTo(Hex(
                    "000000130100000007000000000000000104726f6f6d0a")));
                Assert.That(readModel.Room, Is.Null);
                Assert.That(title.interactable, Is.False);
                Assert.That(capacity.interactable, Is.False);
                Assert.That(create.interactable, Is.False);
                Assert.That(available.interactable, Is.False);
                Assert.That(full.interactable, Is.False);
                router.OnMessage(new RoomCommandResponse(1, 0));
                yield return new WaitUntil(() => create.interactable);
                Assert.That(title.interactable, Is.True);
                Assert.That(capacity.interactable, Is.True);
                Assert.That(create.interactable, Is.True);
                Assert.That(available.interactable, Is.True);
                Assert.That(full.interactable, Is.False);
            }
        }

        [UnityTest]
        public IEnumerator LobbyCapacityInputRejectsOutsideTwoThroughTen()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            LobbyScreenView view =
                UnityEngine.Object.FindFirstObjectByType<LobbyScreenView>();
            var sender = new RecordingSender();
            using (var presenter = new LobbyPresenter(
                       new LobbyRoomReadModel(),
                       new LobbyRoomCommandCoordinator(
                           sender, new RoomCommandCorrelator()),
                       view))
            {
                view.Bind(presenter, CancellationToken.None);
                presenter.Begin();
                InputField title = view.GetComponentsInChildren<InputField>(true)
                    .Single(input => input.name == "RoomTitleInput");
                InputField capacity = view.GetComponentsInChildren<InputField>(true)
                    .Single(input => input.name == "RoomCapacityInput");
                Button create = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "CreateButton");

                title.text = "room";
                capacity.text = "11";
                create.onClick.Invoke();
                yield return null;

                Assert.That(sender.Frames, Is.Empty);
                StringAssert.Contains("2~10", view.StatusCopy);
            }
        }

        [UnityTest]
        public IEnumerator RoomReadyButtonWaitsForServerProjection()
        {
            SceneManager.LoadScene("RoomScene");
            yield return null;

            RoomScreenView view =
                UnityEngine.Object.FindFirstObjectByType<RoomScreenView>();
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            using (var presenter = new RoomPresenter(
                       readModel,
                       new LobbyRoomCommandCoordinator(sender, correlator),
                       new RecordingHostStart(),
                       view))
            {
                view.Bind(presenter, CancellationToken.None);
                presenter.Begin();
                Text status = view.GetComponentsInChildren<Text>(true)
                    .Single(label => label.name == "StatusLabel");
                Button start = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "StartButton");
                Assert.That(view.StatusCopy,
                    Is.EqualTo("서버 방 정보를 기다리는 중입니다."));
                Assert.That(status.text,
                    Is.EqualTo("서버 방 정보를 기다리는 중입니다."));
                router.OnMessage(new LobbyEntrySnapshot(
                    1, 2, "neo", Array.Empty<RoomSummary>()));
                router.OnMessage(new RoomDetailProjection(
                    7,
                    "Treasure Hunters",
                    2,
                    1,
                    2,
                    new[]
                    {
                        new RoomMember(1, 2, "portfolio-player", false),
                        new RoomMember(2, 3, "party-member", true)
                    }));
                Assert.That(view.StatusCopy,
                    Is.EqualTo("플레이어의 준비를 기다리고 있습니다."));
                router.OnMessage(new RoomDetailProjection(
                    7,
                    "Treasure Hunters",
                    2,
                    1,
                    2,
                    new[]
                    {
                        new RoomMember(1, 2, "portfolio-player", true),
                        new RoomMember(2, 3, "party-member", true)
                    }));
                yield return null;
                Assert.That(view.StatusCopy,
                    Is.EqualTo("모든 플레이어가 준비되었습니다."));

                router.OnMessage(new RoomDetailProjection(
                    7,
                    "Treasure Hunters",
                    2,
                    2,
                    3,
                    new[]
                    {
                        new RoomMember(1, 2, "portfolio-player", true),
                        new RoomMember(2, 3, "party-member", true)
                    }));
                Assert.That(view.StatusCopy,
                    Is.EqualTo("모든 플레이어가 준비되었습니다."),
                    "all-ready copy must not require the local player to be host");
                Assert.That(status.text,
                    Is.EqualTo("모든 플레이어가 준비되었습니다."));
                Assert.That(start.interactable, Is.False);
                yield return null;
                Assert.That(view.GetComponentsInChildren<Text>(true)
                    .Where(label => label.name == "Label" &&
                        label.transform.parent.name.StartsWith("MemberRow-"))
                    .Select(label => label.text), Is.EquivalentTo(new[]
                    {
                        "나   portfolio-player   준비 완료",
                        "방장   party-member   준비 완료"
                    }));

                router.OnMessage(new RoomDetailProjection(
                    7,
                    "Treasure Hunters",
                    2,
                    1,
                    2,
                    new[]
                    {
                        new RoomMember(1, 2, "portfolio-player", true)
                    }));
                Assert.That(view.StatusCopy,
                    Is.EqualTo("플레이어의 준비를 기다리고 있습니다."));
                Assert.That(status.text,
                    Is.EqualTo("플레이어의 준비를 기다리고 있습니다."));
                Assert.That(start.interactable, Is.False);

                router.OnMessage(new RoomDetailProjection(
                    7,
                    "Treasure Hunters",
                    2,
                    1,
                    2,
                    new[]
                    {
                        new RoomMember(1, 2, "portfolio-player", true),
                        new RoomMember(2, 3, "party-member", true)
                    }));
                yield return null;

                Button ready = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "ReadyButton");
                Button leave = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "LeaveButton");
                Text title = view.GetComponentsInChildren<Text>(true)
                    .Single(label => label.name == "TitleLabel");
                Text[] members = view.GetComponentsInChildren<Text>(true)
                    .Where(label => label.name == "Label" &&
                        label.transform.parent.name.StartsWith("MemberRow-"))
                    .OrderBy(label => label.text)
                    .ToArray();

                Assert.That(title.text, Is.EqualTo("Treasure Hunters\n2 / 2"));
                Assert.That(members.Select(label => label.text), Is.EquivalentTo(new[]
                {
                    "방장 · 나   portfolio-player   준비 완료",
                    "party-member   준비 완료"
                }));
                Assert.That(members.All(label =>
                    label.color == (Color)new Color32(0x3F, 0x63, 0x2C, 0xFF)),
                    Is.True);
                Assert.That(ready.GetComponentInChildren<Text>(true).text,
                    Is.EqualTo("준비 취소"));
                Assert.That(ready.interactable, Is.True);
                Assert.That(start.interactable, Is.True);
                Assert.That(leave.interactable, Is.True);
                ready.onClick.Invoke();
                yield return null;

                Assert.That(sender.Frames, Has.Count.EqualTo(1));
                Assert.That(view.Snapshot.Members[0].Ready, Is.True);
                Assert.That(ready.interactable, Is.False);
                Assert.That(start.interactable, Is.False);
                Assert.That(leave.interactable, Is.False);
                Assert.That(view.StatusCopy,
                    Is.EqualTo("준비 상태 변경 요청 중입니다."));
                Assert.Throws<InvalidOperationException>(() =>
                    view.SetReadyAsync(false),
                    "pending state must reject a duplicate ready intent");
                router.OnMessage(new RoomCommandResponse(1, 0));
                yield return new WaitUntil(() => view.StatusCopy ==
                    "요청이 접수되었습니다. 서버 상태 반영을 기다립니다.");
                Assert.That(view.StatusCopy,
                    Is.EqualTo("요청이 접수되었습니다. 서버 상태 반영을 기다립니다."));

                router.OnMessage(new RoomDetailProjection(
                    7,
                    "Treasure Hunters",
                    2,
                    1,
                    2,
                    new[]
                    {
                        new RoomMember(1, 2, "portfolio-player", false),
                        new RoomMember(2, 3, "party-member", true)
                    }));
                Assert.That(view.StatusCopy,
                    Is.EqualTo("요청이 접수되었습니다. 서버 상태 반영을 기다립니다."),
                    "a later projection must not overwrite transient command copy");
                Text localMember = view.GetComponentsInChildren<Text>(true)
                    .Single(label => label.text ==
                        "방장 · 나   portfolio-player   준비 전");
                Assert.That(localMember.color,
                    Is.EqualTo((Color)new Color32(0x5A, 0x35, 0x1B, 0xFF)));
                Assert.That(ready.GetComponentInChildren<Text>(true).text,
                    Is.EqualTo("준비"));
                Assert.That(start.interactable, Is.False);
            }
        }

        [UnityTest]
        public IEnumerator RoomLeaveStaysDisabledAfterAcceptedResponse()
        {
            SceneManager.LoadScene("RoomScene");
            yield return null;

            RoomScreenView view =
                UnityEngine.Object.FindFirstObjectByType<RoomScreenView>();
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            using (var presenter = new RoomPresenter(
                       readModel,
                       new LobbyRoomCommandCoordinator(sender, correlator),
                       new RecordingHostStart(),
                       view))
            {
                view.Bind(presenter, CancellationToken.None);
                presenter.Begin();
                router.OnMessage(new LobbyEntrySnapshot(
                    1, 2, "neo", Array.Empty<RoomSummary>()));
                router.OnMessage(Room(firstReady: true, secondReady: true));
                yield return null;

                Button leave = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "LeaveButton");
                leave.onClick.Invoke();
                yield return null;
                Assert.That(sender.Frames, Has.Count.EqualTo(1));

                router.OnMessage(new RoomCommandResponse(1, 0));
                yield return new WaitUntil(() => view.StatusCopy ==
                    "요청이 접수되었습니다. 서버 상태 반영을 기다립니다.");
                yield return null;

                Assert.That(leave.interactable, Is.False,
                    "accepted Leave must remain gated until the Lobby projection arrives");
            }
        }

        [UnityTest]
        public IEnumerator RoomHostConfirmsBeforeSendingKickIntent()
        {
            SceneManager.LoadScene("RoomScene");
            yield return null;

            RoomScreenView view =
                UnityEngine.Object.FindFirstObjectByType<RoomScreenView>();
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            using (var presenter = new RoomPresenter(
                       readModel,
                       new LobbyRoomCommandCoordinator(sender, correlator),
                       new RecordingHostStart(),
                       view))
            {
                view.Bind(presenter, CancellationToken.None);
                presenter.Begin();
                router.OnMessage(new LobbyEntrySnapshot(
                    1, 2, "neo", Array.Empty<RoomSummary>()));
                router.OnMessage(Room(firstReady: false, secondReady: false));
                yield return null;

                Button kick = view.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "KickButton-2");
                Assert.That(kick.gameObject.activeInHierarchy, Is.True);
                Assert.That(kick.interactable, Is.True);
                Assert.That(kick.targetGraphic.raycastTarget, Is.True);
                Text kickLabel = kick.GetComponentInChildren<Text>(true);
                Assert.That(kickLabel.text, Is.EqualTo("강퇴"));
                Assert.That(kickLabel.color.a, Is.GreaterThan(0.9f));
                Assert.That(kickLabel.rectTransform.anchorMin, Is.EqualTo(Vector2.zero));
                Assert.That(kickLabel.rectTransform.anchorMax, Is.EqualTo(Vector2.one));
                Assert.That(kickLabel.rectTransform.offsetMin, Is.EqualTo(Vector2.zero));
                Assert.That(kickLabel.rectTransform.offsetMax, Is.EqualTo(Vector2.zero));

                // 이 버튼은 런타임 생성이라 프리팹 에셋 테스트의 fontSize >= 24 와
                // Selectable 높이 규칙이 닿지 않는다. 그래서 여기서 직접 잠근다.
                // 행(ListRow) 높이가 48 이므로 54 는 들어가지 않는다.
                Assert.That(kickLabel.fontSize, Is.GreaterThanOrEqualTo(24));
                Rect kickRect = kick.GetComponent<RectTransform>().rect;
                Assert.That(kickRect.height, Is.GreaterThanOrEqualTo(40f));
                Assert.That(kickRect.width, Is.GreaterThanOrEqualTo(96f));

                // Kenney 5px border 는 기본 배율에서 15.6 unit 이라 높이 40 을 프레임으로
                // 채운다. 배율을 올려 border 를 원본 크기로 되돌려야 라벨 자리가 남는다.
                Assert.That(kick.image.pixelsPerUnitMultiplier,
                    Is.GreaterThan(1f),
                    "9슬라이스 border 가 버튼 내부를 삼키지 않아야 한다");
                kick.onClick.Invoke();
                Assert.That(sender.Frames, Is.Empty);

                GameObject confirmation = view.GetComponentsInChildren<Transform>(true)
                    .Single(child => child.name == "ActionDialog")
                    .gameObject;
                Assert.That(confirmation.activeSelf, Is.True);
                Button confirm = confirmation.GetComponentsInChildren<Button>(true)
                    .Single(button => button.name == "ActionConfirmButton");
                confirm.onClick.Invoke();
                yield return null;

                Assert.That(sender.Frames, Has.Count.EqualTo(1));
                Assert.That(sender.Frames[0], Is.EqualTo(Hex(
                    "0000001d010000000b000000000000000100000000000000020000000000000003")));
            }
        }

        [UnityTest]
        public IEnumerator ServerLobbyProjectionDistinguishesVoluntaryLeaveFromKick()
        {
            var sender = new RecordingSender();
            var correlator = new RoomCommandCorrelator();
            var readModel = new LobbyRoomReadModel();
            var router = new LobbyRoomMessageRouter(correlator, readModel);
            var commands = new LobbyRoomCommandCoordinator(sender, correlator);
            int kicked = 0;
            readModel.Kicked += () => kicked++;

            router.OnMessage(new LobbyEntrySnapshot(
                1, 2, "neo", Array.Empty<RoomSummary>()));
            router.OnMessage(Room(firstReady: false, secondReady: false));
            Task leave = commands.LeaveAsync(CancellationToken.None);
            yield return null;
            router.OnMessage(new RoomCommandResponse(1, 0));
            yield return new WaitUntil(() => leave.IsCompleted);
            router.OnMessage(new LobbyRoomListUpdate(Array.Empty<RoomSummary>()));
            Assert.That(kicked, Is.Zero);

            router.OnMessage(Room(firstReady: false, secondReady: false));
            router.OnMessage(new LobbyRoomListUpdate(Array.Empty<RoomSummary>()));
            Assert.That(kicked, Is.EqualTo(1));
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

        private static LobbyPresentationSnapshot LobbySnapshot()
        {
            return new LobbyPresentationSnapshot(
                1,
                "portfolio-player",
                new[]
                {
                    new LobbyRoomSummaryView(7, "Treasure Hunters", 1, 2),
                    new LobbyRoomSummaryView(8, "Full Expedition", 2, 2)
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

        private sealed class ThrowingSender : ITcpCommandSender
        {
            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                return Task.FromException(new InvalidOperationException("send failed"));
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
