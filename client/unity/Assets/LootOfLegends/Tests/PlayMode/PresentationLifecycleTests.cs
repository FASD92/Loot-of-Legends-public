using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation;
using LootOfLegends.Presentation.Collection;
using LootOfLegends.Presentation.Common;
using LootOfLegends.Presentation.Lobby;
using LootOfLegends.Protocol;
using LootOfLegends.Session;
using LootOfLegends.Transport;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class PresentationLifecycleTests
    {
        [UnityTest]
        public IEnumerator SceneReloadKeepsOnePersistentFailureOverlay()
        {
            SceneManager.LoadScene("LoginScene");
            yield return null;
            SafeFailureTextView first =
                UnityEngine.Object.FindFirstObjectByType<SafeFailureTextView>();
            Assert.That(first, Is.Not.Null);

            SceneManager.LoadScene("LobbyScene");
            yield return null;

            SafeFailureTextView[] views = UnityEngine.Object.FindObjectsByType<
                SafeFailureTextView>(
                FindObjectsInactive.Include,
                FindObjectsSortMode.None);
            Assert.That(views, Has.Length.EqualTo(1));
            Assert.That(views[0], Is.SameAs(first));
            Assert.That(views[0].GetComponents<Canvas>(), Has.Length.EqualTo(1));
            Assert.That(UnityEngine.Object.FindObjectsByType<EventSystem>(
                FindObjectsInactive.Include,
                FindObjectsSortMode.None), Has.Length.EqualTo(1));
        }

        [UnityTest]
        public IEnumerator LobbyReloadReplacesPresenterWithoutDuplicateSubscription()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            var session = new PlayerSessionReadModel();
            session.BeginAuthentication();
            session.Apply(new WelcomeSession(1, 1, 1, 0, "neo"));
            var lobby = new LobbyRoomReadModel();
            lobby.Apply(new LobbyEntrySnapshot(
                1, 1, "neo", Array.Empty<RoomSummary>()));
            var runtime = new PlayerFlowPresentationRuntime(
                session,
                lobby,
                new StubRoomCommands(),
                new BattleLoadCoordinator(new StubSender(), new BattleResponseCorrelator()),
                new BattleLoadReadModel(),
                new BattleResultReadModel(),
                new StubCollectionApi(),
                new CollectionReadModel(),
                () => null,
                CancellationToken.None);
            try
            {
                LobbyScreenView first =
                    UnityEngine.Object.FindFirstObjectByType<LobbyScreenView>();
                Assert.That(first.Snapshot.Nickname, Is.EqualTo("neo"));
                Assert.That(SubscriberCount(lobby), Is.EqualTo(1));

                SceneManager.LoadScene("LobbyScene");
                yield return null;

                LobbyScreenView second =
                    UnityEngine.Object.FindFirstObjectByType<LobbyScreenView>();
                Assert.That(second, Is.Not.SameAs(first));
                Assert.That(second.Snapshot.Nickname, Is.EqualTo("neo"));
                Assert.That(SubscriberCount(lobby), Is.EqualTo(1));

                lobby.Apply(new LobbyRoomListUpdate(new[]
                {
                    new RoomSummary(7, "room", 1, 2)
                }));
                Assert.That(second.Snapshot.Rooms, Has.Count.EqualTo(1));
            }
            finally
            {
                runtime.Dispose();
            }

            Assert.That(SubscriberCount(lobby), Is.Zero);
        }

        [UnityTest]
        public IEnumerator LobbyReloadSerializesCollectionRefreshes()
        {
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            var session = new PlayerSessionReadModel();
            session.BeginAuthentication();
            session.Apply(new WelcomeSession(1, 1, 1, 0, "neo"));
            var lobby = new LobbyRoomReadModel();
            lobby.Apply(new LobbyEntrySnapshot(
                1, 1, "neo", Array.Empty<RoomSummary>()));
            var api = new DeferredCollectionApi();
            var runtime = new PlayerFlowPresentationRuntime(
                session,
                lobby,
                new StubRoomCommands(),
                new BattleLoadCoordinator(new StubSender(), new BattleResponseCorrelator()),
                new BattleLoadReadModel(),
                new BattleResultReadModel(),
                api,
                new CollectionReadModel(),
                () => null,
                CancellationToken.None);
            try
            {
                Assert.That(api.Calls, Is.EqualTo(1));

                SceneManager.LoadScene("LobbyScene");
                yield return null;

                Assert.That(api.Calls, Is.EqualTo(1),
                    "scene reload must not overlap Collection refreshes");
                api.Complete(0, new CollectionSnapshot(
                    Array.Empty<CollectionItem>(), 100, 0));
                yield return null;
                runtime.Tick();
                Assert.That(api.Calls, Is.EqualTo(2));

                api.Complete(1, new CollectionSnapshot(
                    Array.Empty<CollectionItem>(), 200, 0));
                yield return null;
                runtime.Tick();
                CollectionScreenView view =
                    UnityEngine.Object.FindFirstObjectByType<CollectionScreenView>();
                Assert.That(view.Snapshot.Wallet, Is.EqualTo(200));
            }
            finally
            {
                runtime.Dispose();
            }
        }

        [UnityTest]
        public IEnumerator KickedRoomMemberSeesDismissibleLobbyNotice()
        {
            SceneManager.LoadScene("RoomScene");
            yield return null;

            var session = new PlayerSessionReadModel();
            session.BeginAuthentication();
            session.Apply(new WelcomeSession(1, 1, 1, 0, "neo"));
            var lobby = new LobbyRoomReadModel();
            lobby.Apply(new LobbyEntrySnapshot(
                1, 1, "neo", Array.Empty<RoomSummary>()));
            lobby.Apply(new RoomDetailProjection(
                7,
                "room",
                2,
                2,
                1,
                new[]
                {
                    new RoomMember(1, 1, "neo", false),
                    new RoomMember(2, 1, "host", false)
                }));
            var correlator = new RoomCommandCorrelator();
            var router = new LobbyRoomMessageRouter(correlator, lobby);
            var runtime = new PlayerFlowPresentationRuntime(
                session,
                lobby,
                new StubRoomCommands(),
                new BattleLoadCoordinator(new StubSender(), new BattleResponseCorrelator()),
                new BattleLoadReadModel(),
                new BattleResultReadModel(),
                new StubCollectionApi(),
                new CollectionReadModel(),
                () => null,
                CancellationToken.None);
            try
            {
                router.OnMessage(new LobbyRoomListUpdate(Array.Empty<RoomSummary>()));
                runtime.Tick();
                yield return null;

                Assert.That(SceneManager.GetActiveScene().name, Is.EqualTo("LobbyScene"));
                ActionDialogView notice =
                    UnityEngine.Object.FindFirstObjectByType<ActionDialogView>();
                Assert.That(notice, Is.Not.Null);
                Assert.That(notice.IsVisible, Is.True);
                Assert.That(notice.Message, Is.EqualTo("방장에 의해 강퇴되었습니다."));
                Assert.That(notice.ConfirmCopy, Is.EqualTo("확인"));

                notice.Confirm();
                Assert.That(notice.IsVisible, Is.False);
            }
            finally
            {
                runtime.Dispose();
            }
        }

        private static int SubscriberCount(LobbyRoomReadModel model)
        {
            FieldInfo changed = typeof(LobbyRoomReadModel).GetField(
                "Changed",
                BindingFlags.Instance | BindingFlags.NonPublic);
            var listeners = (Delegate)changed.GetValue(model);
            return listeners?.GetInvocationList().Length ?? 0;
        }

        private sealed class StubSender : ITcpCommandSender
        {
            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                return Task.CompletedTask;
            }
        }

        private sealed class StubRoomCommands : ILobbyRoomCommands
        {
            public Task<RoomCommandResult> CreateAsync(
                string title,
                byte capacity,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }

            public Task<RoomCommandResult> JoinAsync(
                ulong roomId,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }

            public Task<RoomCommandResult> LeaveAsync(CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }

            public Task<RoomCommandResult> SetReadyAsync(
                bool ready,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }

            public Task<RoomCommandResult> KickAsync(
                ulong targetSessionId,
                ulong targetSessionGeneration,
                CancellationToken cancellationToken)
            {
                return Task.FromResult(RoomCommandResult.Ok);
            }
        }

        private sealed class StubCollectionApi : ICollectionApi
        {
            public Task<CollectionSnapshot> FetchAsync(
                CancellationToken cancellationToken)
            {
                return Task.FromResult(new CollectionSnapshot(
                    Array.Empty<CollectionItem>(),
                    0,
                    0));
            }
        }

        private sealed class DeferredCollectionApi : ICollectionApi
        {
            private readonly List<TaskCompletionSource<CollectionSnapshot>> requests =
                new List<TaskCompletionSource<CollectionSnapshot>>();

            public int Calls => requests.Count;

            public Task<CollectionSnapshot> FetchAsync(
                CancellationToken cancellationToken)
            {
                var completion = new TaskCompletionSource<CollectionSnapshot>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                requests.Add(completion);
                return completion.Task;
            }

            public void Complete(int index, CollectionSnapshot snapshot)
            {
                requests[index].SetResult(snapshot);
            }
        }
    }
}
