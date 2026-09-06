using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Presentation.Collection;
using LootOfLegends.Presentation.Common;
using LootOfLegends.Presentation.FinalResult;
using LootOfLegends.Presentation.Lobby;
using LootOfLegends.Presentation.Login;
using LootOfLegends.Presentation.Room;
using LootOfLegends.Session;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.Presentation
{
    public sealed class PlayerFlowPresentationRuntime : IDisposable
    {
        private readonly PlayerSessionReadModel session;
        private readonly LobbyRoomReadModel lobbyRoom;
        private readonly ILobbyRoomCommands roomCommands;
        private readonly BattleLoadCoordinator battleCommands;
        private readonly BattleLoadReadModel battleLoad;
        private readonly BattleResultReadModel battleResult;
        private readonly ICollectionApi collectionApi;
        private readonly CollectionReadModel collection;
        private readonly Func<ArenaInputBinding> arena;
        private readonly CancellationToken cancellationToken;
        private readonly PlayerFlowKeyboardInput keyboard;
        private readonly SafeFailureTextView safeView;
        private readonly SafeFailurePresenter safeFailure;
        private readonly BattleLoadFailurePresenter loadFailure;
        private readonly BattleRecoveryPresenter recovery;
        private LobbyPresenter lobbyPresenter;
        private RoomPresenter roomPresenter;
        private ArenaPresenter arenaPresenter;
        private FinalResultPresenter resultPresenter;
        private CollectionPresenter collectionPresenter;
        private CollectionScreenView collectionView;
        private Task collectionRefresh;
        private bool collectionRefreshQueued;
        private bool kickNoticePending;
        private Task loadCompletion;
        private ulong requestedLoadBattleId;
        private string configuredScene = string.Empty;
        private bool disposed;

        public PlayerFlowPresentationRuntime(
            PlayerSessionReadModel session,
            LobbyRoomReadModel lobbyRoom,
            ILobbyRoomCommands roomCommands,
            BattleLoadCoordinator battleCommands,
            BattleLoadReadModel battleLoad,
            BattleResultReadModel battleResult,
            ICollectionApi collectionApi,
            CollectionReadModel collection,
            Func<ArenaInputBinding> arena,
            CancellationToken cancellationToken)
        {
            this.session = session ?? throw new ArgumentNullException(nameof(session));
            this.lobbyRoom = lobbyRoom ?? throw new ArgumentNullException(nameof(lobbyRoom));
            this.roomCommands = roomCommands ??
                throw new ArgumentNullException(nameof(roomCommands));
            this.battleCommands = battleCommands ??
                throw new ArgumentNullException(nameof(battleCommands));
            this.battleLoad = battleLoad ??
                throw new ArgumentNullException(nameof(battleLoad));
            this.battleResult = battleResult ??
                throw new ArgumentNullException(nameof(battleResult));
            this.collectionApi = collectionApi ??
                throw new ArgumentNullException(nameof(collectionApi));
            this.collection = collection ??
                throw new ArgumentNullException(nameof(collection));
            this.arena = arena ?? throw new ArgumentNullException(nameof(arena));
            this.cancellationToken = cancellationToken;

            keyboard = new PlayerFlowKeyboardInput(
                roomCommands,
                arena,
                ShowStatus);

            safeView = UnityEngine.Object.FindFirstObjectByType<SafeFailureTextView>();
            if (safeView == null)
            {
                throw new InvalidOperationException(
                    "The active Scene must provide SafeFailureTextView");
            }
            safeView.HideBlockingMessage();
            safeFailure = new SafeFailurePresenter(
                session,
                safeView,
                new UnityLoginNavigation());
            loadFailure = new BattleLoadFailurePresenter(
                battleLoad,
                safeView,
                new UnityRoomNavigation());
            recovery = new BattleRecoveryPresenter(
                battleResult,
                safeView,
                new UnityLobbyNavigation());

            safeFailure.Begin();
            loadFailure.Begin();
            lobbyRoom.Kicked += OnKicked;
            SceneManager.sceneLoaded += OnSceneLoaded;
            Configure(SceneManager.GetActiveScene().name);
        }

        public void Tick()
        {
            if (disposed)
            {
                return;
            }
            ObserveCollectionRefresh();
            Observe(ref loadCompletion, "arena_load");
            recovery.Render();

            string active = SceneManager.GetActiveScene().name;
            string desired = DesiredScene(active);
            if (!string.IsNullOrEmpty(desired) && active != desired)
            {
                SceneManager.LoadScene(desired);
                active = desired;
            }
            if (configuredScene != active ||
                (active == "ArenaScene" && arenaPresenter == null && arena() != null))
            {
                Configure(active);
            }

            BeginArenaLoadCompletion(active);
            keyboard.Tick(cancellationToken);
            arenaPresenter?.Render();
            resultPresenter?.Render();
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }
            disposed = true;
            SceneManager.sceneLoaded -= OnSceneLoaded;
            lobbyRoom.Kicked -= OnKicked;
            lobbyPresenter?.Dispose();
            roomPresenter?.Dispose();
            safeFailure.Dispose();
            loadFailure.Dispose();
        }

        private string DesiredScene(string activeScene)
        {
            if (session.State != PlayerSessionState.Authenticated)
            {
                return "LoginScene";
            }
            if (battleLoad.IsWaiting || battleLoad.IsGameplayActive ||
                (battleResult.HasFinalResult && activeScene == "ArenaScene" &&
                    lobbyRoom.IsInRoom))
            {
                return "ArenaScene";
            }
            return lobbyRoom.IsInRoom ? "RoomScene" : "LobbyScene";
        }

        private void OnSceneLoaded(Scene scene, LoadSceneMode mode)
        {
            Configure(scene.name);
        }

        private void Configure(string sceneName)
        {
            lobbyPresenter?.Dispose();
            lobbyPresenter = null;
            roomPresenter?.Dispose();
            roomPresenter = null;
            arenaPresenter = null;
            resultPresenter = null;
            collectionPresenter = null;
            collectionView = null;
            if (sceneName != "LobbyScene")
            {
                collectionRefreshQueued = false;
            }
            configuredScene = sceneName;

            if (sceneName == "LobbyScene")
            {
                LobbyScreenView lobbyView =
                    UnityEngine.Object.FindFirstObjectByType<LobbyScreenView>();
                if (lobbyView != null)
                {
                    lobbyPresenter = new LobbyPresenter(
                        lobbyRoom, roomCommands, lobbyView);
                    lobbyView.Bind(lobbyPresenter, cancellationToken);
                    lobbyPresenter.Begin();
                    if (kickNoticePending)
                    {
                        lobbyView.ShowKickedNotice();
                        kickNoticePending = false;
                    }
                }
                collectionView =
                    UnityEngine.Object.FindFirstObjectByType<CollectionScreenView>();
                if (collectionView != null)
                {
                    collectionPresenter = new CollectionPresenter(
                        collection, collectionApi, collectionView);
                    collectionView.Bind(RequestCollectionRefresh);
                    collectionPresenter.Render();
                    RequestCollectionRefresh();
                }
            }
            else if (sceneName == "RoomScene")
            {
                RoomScreenView roomView =
                    UnityEngine.Object.FindFirstObjectByType<RoomScreenView>();
                if (roomView != null)
                {
                    roomPresenter = new RoomPresenter(
                        lobbyRoom,
                        roomCommands,
                        new BattleHostStartAction(battleCommands),
                        roomView);
                    roomView.Bind(roomPresenter, cancellationToken);
                    roomPresenter.Begin();
                }
            }
            else if (sceneName == "ArenaScene")
            {
                ulong hostSessionId = 0;
                RoomPresentationSnapshot room = lobbyRoom.Room;
                if (room != null)
                {
                    foreach (RoomMemberPresentation member in room.Members)
                    {
                        if (member.IsHost)
                        {
                            hostSessionId = member.SessionId;
                            break;
                        }
                    }
                }
                ArenaInputBinding binding = arena();
                ArenaScreenView arenaView =
                    UnityEngine.Object.FindFirstObjectByType<ArenaScreenView>();
                if (binding != null && arenaView != null)
                {
                    arenaView.SetPlayerContext(session.SessionId, hostSessionId);
                    arenaPresenter = new ArenaPresenter(
                        binding.Presentation,
                        binding.Input,
                        arenaView);
                    arenaView.Bind(arenaPresenter, cancellationToken);
                }
                FinalResultScreenView resultView =
                    UnityEngine.Object.FindFirstObjectByType<FinalResultScreenView>();
                if (resultView != null)
                {
                    resultView.SetPlayerContext(session.SessionId, hostSessionId);
                    resultPresenter = new FinalResultPresenter(
                        battleResult,
                        resultView,
                        new UnityRoomReturnNavigation(),
                        roomCommands.LeaveAsync);
                    resultView.Bind(
                        resultPresenter,
                        cancellationToken);
                }
            }
        }

        private void RequestCollectionRefresh()
        {
            if (collectionPresenter == null || collectionView == null)
            {
                return;
            }
            if (collectionRefresh != null)
            {
                collectionRefreshQueued = true;
                return;
            }
            collectionRefreshQueued = false;
            collectionRefresh = collectionPresenter.RefreshAsync(cancellationToken);
        }

        private void OnKicked()
        {
            kickNoticePending = true;
        }

        private void ObserveCollectionRefresh()
        {
            if (collectionRefresh == null || !collectionRefresh.IsCompleted)
            {
                return;
            }
            Observe(ref collectionRefresh, "collection");
            if (!collectionRefreshQueued || configuredScene != "LobbyScene" ||
                collectionPresenter == null || collectionView == null)
            {
                return;
            }
            collectionPresenter.Render();
            RequestCollectionRefresh();
        }

        private void BeginArenaLoadCompletion(string sceneName)
        {
            ArenaInputBinding binding = arena();
            if (sceneName != "ArenaScene" || !battleLoad.IsWaiting ||
                battleLoad.BattleInstanceId == 0 ||
                battleLoad.BattleInstanceId == requestedLoadBattleId ||
                loadCompletion != null ||
                binding == null || !binding.IsTransportReady)
            {
                return;
            }
            requestedLoadBattleId = battleLoad.BattleInstanceId;
            loadCompletion = CompleteArenaLoadAsync(
                battleLoad.RoomId,
                battleLoad.BattleInstanceId);
        }

        private async Task CompleteArenaLoadAsync(ulong roomId, ulong battleInstanceId)
        {
            BattleCommandOutcome outcome =
                await battleCommands.CompleteArenaLoadOutcomeAsync(
                    roomId,
                    battleInstanceId,
                    cancellationToken);
            if (outcome != BattleCommandOutcome.Ok)
            {
                ShowStatus("아레나 진입 요청을 완료하지 못했습니다.");
                Debug.LogWarning(
                    "Player flow arena load request was rejected safely: " + outcome);
            }
        }

        private static void Observe(ref Task task, string stage)
        {
            if (task == null || !task.IsCompleted)
            {
                return;
            }
            if (task.IsFaulted)
            {
                string failure = task.Exception?.GetBaseException().GetType().Name ??
                    "UnknownException";
                Debug.LogWarning(
                    "Player flow request failed safely: " + stage + " " + failure);
            }
            task = null;
        }

        private static void ShowStatus(string copy)
        {
            LoginStatusTextView login =
                UnityEngine.Object.FindFirstObjectByType<LoginStatusTextView>();
            if (login != null)
            {
                login.ShowStatus(copy);
            }
        }
    }
}
