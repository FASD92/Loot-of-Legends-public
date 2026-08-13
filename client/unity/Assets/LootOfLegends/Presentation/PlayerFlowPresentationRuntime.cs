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
        private Task collectionRefresh;
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

            var hostStart = new BattleHostStartAction(battleCommands);
            keyboard = new PlayerFlowKeyboardInput(
                lobbyRoom,
                roomCommands,
                hostStart,
                arena,
                ShowStatus);

            var overlay = new GameObject("PlayerFlowSafeFailure");
            UnityEngine.Object.DontDestroyOnLoad(overlay);
            safeView = overlay.AddComponent<SafeFailureTextView>();
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
            SceneManager.sceneLoaded += OnSceneLoaded;
            Configure(SceneManager.GetActiveScene().name);
        }

        public void Tick()
        {
            if (disposed)
            {
                return;
            }
            Observe(ref collectionRefresh, "collection");
            Observe(ref loadCompletion, "arena_load");
            recovery.Render();

            string desired = DesiredScene();
            string active = SceneManager.GetActiveScene().name;
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
            lobbyPresenter?.Dispose();
            roomPresenter?.Dispose();
            safeFailure.Dispose();
            loadFailure.Dispose();
            if (safeView != null)
            {
                UnityEngine.Object.Destroy(safeView.gameObject);
            }
        }

        private string DesiredScene()
        {
            if (session.State != PlayerSessionState.Authenticated)
            {
                return "LoginScene";
            }
            if (battleLoad.IsWaiting || battleLoad.IsGameplayActive ||
                (battleResult.HasFinalResult && !battleResult.IsReadyForRematch))
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
            configuredScene = sceneName;

            if (sceneName == "LobbyScene")
            {
                LobbyTextView lobbyView =
                    UnityEngine.Object.FindFirstObjectByType<LobbyTextView>();
                if (lobbyView != null)
                {
                    lobbyPresenter = new LobbyPresenter(
                        lobbyRoom, roomCommands, lobbyView);
                    lobbyPresenter.Begin();
                }
                CollectionTextView collectionView =
                    UnityEngine.Object.FindFirstObjectByType<CollectionTextView>();
                if (collectionView != null)
                {
                    collectionPresenter = new CollectionPresenter(
                        collection, collectionApi, collectionView);
                    collectionRefresh = collectionPresenter.RefreshAsync(cancellationToken);
                }
            }
            else if (sceneName == "RoomScene")
            {
                RoomTextView roomView =
                    UnityEngine.Object.FindFirstObjectByType<RoomTextView>();
                if (roomView != null)
                {
                    roomPresenter = new RoomPresenter(
                        lobbyRoom,
                        roomCommands,
                        new BattleHostStartAction(battleCommands),
                        roomView);
                    roomPresenter.Begin();
                }
            }
            else if (sceneName == "ArenaScene")
            {
                ArenaInputBinding binding = arena();
                ArenaTextView arenaView =
                    UnityEngine.Object.FindFirstObjectByType<ArenaTextView>();
                if (binding != null && arenaView != null)
                {
                    arenaPresenter = new ArenaPresenter(
                        binding.Presentation,
                        binding.Input,
                        arenaView);
                }
                FinalResultTextView resultView =
                    UnityEngine.Object.FindFirstObjectByType<FinalResultTextView>();
                if (resultView != null)
                {
                    resultPresenter = new FinalResultPresenter(
                        battleResult,
                        resultView,
                        new UnityRoomReturnNavigation());
                }
            }
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
                ShowStatus("Arena 진입 요청을 완료하지 못했습니다.");
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
