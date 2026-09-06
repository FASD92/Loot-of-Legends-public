using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation;
using LootOfLegends.Presentation.Common;
using LootOfLegends.Presentation.Login;
using LootOfLegends.Protocol;
using LootOfLegends.Session;
using LootOfLegends.Transport;
using LootOfLegends.Transport.Rudp;
using UnityEngine;

namespace LootOfLegends.Bootstrap
{
    public sealed class ProductPlayerFlowBootstrap : MonoBehaviour, IMainThreadDispatcher
    {
        private const int DispatcherCapacity = 1024;
        private const int DispatcherBudget = 256;
        private readonly object dispatchGate = new object();
        private readonly Queue<Action> pendingActions = new Queue<Action>();
        private readonly List<IDisposable> subscriptions = new List<IDisposable>();
        private CancellationTokenSource shutdown;
        private HttpClient http;
        private TcpClient tcp;
        private UdpClient udp;
        private Task startup;
        private Task arenaTick;
        private Task arenaBind;
        private Task reconnect;
        private PlayerFlowTransportLifetime transportLifetime;
        private RudpReliableOutbound reliableOutbound;
        private PlayerSessionReadModel session;
        private BattleLoadReadModel battleLoad;
        private BattleResultReadModel battleResult;
        private ArenaClientRuntime arenaRuntime;
        private ArenaInputBinding arenaBinding;
        private TypedServerEventRouter router;
        private ITcpCommandSender tcpSender;
        private RudpInboundPump inbound;
        private IDisposable arenaRudpSubscription;
        private GameCredentialHttpApi gameCredentialApi;
        private IPEndPoint gameDatagramEndpoint;
        private string gameHost;
        private int gameTcpPort;
        private BattleSessionReconnectClient reconnectClient;
        private PlayerFlowPresentationRuntime presentation;
#if DEVELOPMENT_BUILD || UNITY_EDITOR
        private DevelopmentPlayerFlowDriver evidenceDriver;
        private bool snapshotAppliedEvidenceReported;
#endif
        private ulong nextTransportRequestId = 1000000;
        private ulong activeBattleId;
        private bool startupFailureHandled;
        private bool tcpFailureHandled;
        private bool rudpFailureHandled;
        private bool reconnectAwaitingRudp;
        private BattleRecoveryResumeProof battleRecoveryProof;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        private static void Install()
        {
            if (Environment.GetCommandLineArgs().Any(argument => argument == "-runTests"))
            {
                return;
            }
            var root = new GameObject("ProductPlayerFlowBootstrap");
            DontDestroyOnLoad(root);
            root.AddComponent<ProductPlayerFlowBootstrap>();
        }

        private void Awake()
        {
            shutdown = new CancellationTokenSource();
        }

        private void Start()
        {
            if (!ProductConfiguration.TryRead(
                    Environment.GetCommandLineArgs(),
                    out ProductConfiguration configuration))
            {
                ShowLogin("실행 연결 설정이 필요합니다.");
                return;
            }
            startup = StartFlowAsync(configuration, shutdown.Token);
        }

        public void Post(Action action)
        {
            if (action == null)
            {
                throw new ArgumentNullException(nameof(action));
            }
            lock (dispatchGate)
            {
                if (pendingActions.Count >= DispatcherCapacity)
                {
                    throw new InvalidOperationException(
                        "Player flow main-thread queue is overloaded");
                }
                pendingActions.Enqueue(action);
            }
        }

        private void Update()
        {
            DrainMainThreadActions();
            ObserveStartup();
            HandlePortfolioDisconnectShortcut(Input.GetKeyDown(KeyCode.F8));
            ObserveTransport();
            ObserveReconnect();
            ObserveArenaTasks();

            if (reconnectAwaitingRudp && arenaRuntime != null &&
                arenaRuntime.IsTransportReady)
            {
                reconnectAwaitingRudp = false;
                battleLoad?.SetReconnectLocked(false);
                HideBlockingFailure();
            }

            if (arenaRuntime != null && arenaTick == null && reconnect == null)
            {
                arenaTick = TickArenaAsync(shutdown.Token);
            }
            presentation?.Tick();
#if DEVELOPMENT_BUILD || UNITY_EDITOR
            evidenceDriver?.Tick();
#endif
        }

        private void HandlePortfolioDisconnectShortcut(bool requested)
        {
            if (!requested || reconnect != null || !CanResumeBattle())
            {
                return;
            }
            tcp?.Close();
        }

        private async Task StartFlowAsync(
            ProductConfiguration configuration,
            CancellationToken cancellationToken)
        {
            http = new HttpClient
            {
                Timeout = TimeSpan.FromSeconds(15)
            };
            var metaSession = new MetaSessionState();
#if DEVELOPMENT_BUILD || UNITY_EDITOR
            if (configuration.DevelopmentMetaSession != null)
            {
                metaSession.Accept(configuration.DevelopmentMetaSession);
            }
            else
#endif
            {
                ISystemBrowser browser = new UnitySystemBrowser();
#if DEVELOPMENT_BUILD || UNITY_EDITOR
                if (configuration.IsDevelopmentEvidence)
                {
                    browser = new DevelopmentLoopbackBrowser();
                }
#endif
                var login = new DesktopLoginCoordinator(
                    new DesktopAuthHttpApi(http, configuration.MetaBaseUri),
                    new TcpLoopbackAuthListenerFactory(),
                    browser,
                    metaSession);
                ShowLogin("브라우저 인증 완료를 기다리고 있습니다.");
                await login.SignInAsync(cancellationToken);
            }

            gameCredentialApi = new GameCredentialHttpApi(
                    http,
                    configuration.MetaBaseUri,
                    metaSession.Authorize);
            IssuedGameCredential credential = await gameCredentialApi
                .IssueAsync(cancellationToken);

            tcp = new TcpClient(AddressFamily.InterNetwork);
            gameHost = configuration.GameHost;
            gameTcpPort = configuration.TcpPort;
            using (cancellationToken.Register(tcp.Close))
            {
                await tcp.ConnectAsync(
                    configuration.GameHost,
                    configuration.TcpPort);
            }

            session = new PlayerSessionReadModel();
            var lobbyRoom = new LobbyRoomReadModel();
            battleLoad = new BattleLoadReadModel();
            battleResult = new BattleResultReadModel();
            var roomCorrelator = new RoomCommandCorrelator();
            var battleCorrelator = new BattleResponseCorrelator();
            var roomRouter = new LobbyRoomMessageRouter(roomCorrelator, lobbyRoom);
            var completion = new BattleCompletionRouter(
                battleCorrelator,
                battleLoad,
                battleResult);
            router = new TypedServerEventRouter(this);
            subscriptions.Add(router.Subscribe((ISessionInboundMessageSink)session));
            subscriptions.Add(router.Subscribe((ILobbyRoomInboundMessageSink)roomRouter));
            subscriptions.Add(router.Subscribe((ITcpInboundMessageSink)completion));
            subscriptions.Add(router.Subscribe((ILobbyRoomInboundMessageSink)completion));
            subscriptions.Add(router.Subscribe((IFinalResultInboundMessageSink)completion));
            subscriptions.Add(router.Subscribe((IBattleRecoveryInboundMessageSink)completion));

            NetworkStream stream = tcp.GetStream();
            BindTcpSender(new TcpCommandSender(stream));
            transportLifetime = new PlayerFlowTransportLifetime(
                cancellationToken,
                this,
                HandleConfirmedTransportFailure);
            transportLifetime.StartTcp(new TcpInboundPump(stream, router));
            await new GameSessionAuthenticator(tcpSender, session)
                .AuthenticateAsync(credential.Credential, cancellationToken);

            IPAddress gameAddress = await ResolveAddressAsync(
                configuration.GameHost,
                cancellationToken);
            gameDatagramEndpoint = new IPEndPoint(
                gameAddress,
                configuration.UdpPort);
            udp = new UdpClient(AddressFamily.InterNetwork);
            udp.Connect(gameDatagramEndpoint);
            inbound = new RudpInboundPump(
                udp,
                gameDatagramEndpoint,
                session.SessionId,
                session.SessionGeneration);
            reliableOutbound = new RudpReliableOutbound(
                new UdpDatagramSender(udp),
                inbound);
            transportLifetime.StartRudp(inbound);

            var roomCommands = new LobbyRoomCommandCoordinator(
                tcpSender,
                roomCorrelator);
            var battleCommands = new BattleLoadCoordinator(
                tcpSender,
                battleCorrelator);
            var collection = new CollectionReadModel();
            var collectionApi = new CollectionHttpApi(
                http,
                configuration.MetaBaseUri,
                metaSession.Authorize);

            battleLoad.Changed += EnsureArenaRuntime;
            presentation = new PlayerFlowPresentationRuntime(
                session,
                lobbyRoom,
                roomCommands,
                battleCommands,
                battleLoad,
                battleResult,
                collectionApi,
                collection,
                () => arenaBinding,
                cancellationToken);
            reconnectClient = new BattleSessionReconnectClient(
                tcpSender,
                async token => (await gameCredentialApi.IssueAsync(token)
                    .ConfigureAwait(false)).Credential,
                new StopwatchReconnectClock(),
                ApplyResumeSnapshot,
                this,
                (requestId, welcome) => session != null &&
                    session.ApplyResumedWelcome(requestId, welcome));
            subscriptions.Add(router.Subscribe(
                (IBattleResumeInboundMessageSink)reconnectClient));
            subscriptions.Add(router.Subscribe(
                (ISessionInboundMessageSink)reconnectClient));
#if DEVELOPMENT_BUILD || UNITY_EDITOR
            if (configuration.IsAutomatedEvidence)
            {
                evidenceDriver = new DevelopmentPlayerFlowDriver(
                    configuration.EvidenceRole,
                    lobbyRoom,
                    roomCommands,
                    new BattleHostStartAction(battleCommands),
                    battleLoad,
                    battleResult,
                    () => arenaBinding,
                    collectionApi,
                    collection,
                    cancellationToken,
                    configuration.IsCrashContinuityEvidence,
                    () => session.SessionGeneration);
            }
#endif
            ShowLogin("게임 서버 인증이 완료되었습니다.");
        }

        private void EnsureArenaRuntime()
        {
            if (battleLoad == null || battleLoad.BattleInstanceId == 0 ||
                battleLoad.BattleInstanceId == activeBattleId ||
                tcpSender == null || reliableOutbound == null || inbound == null)
            {
                return;
            }

            arenaRudpSubscription?.Dispose();
            arenaRudpSubscription = null;
            activeBattleId = battleLoad.BattleInstanceId;
            if (battleRecoveryProof == null ||
                battleRecoveryProof.RoomId != battleLoad.RoomId ||
                battleRecoveryProof.BattleInstanceId != activeBattleId ||
                battleRecoveryProof.SessionId != session.SessionId)
            {
                battleRecoveryProof = new BattleRecoveryResumeProof(
                    battleLoad.RoomId,
                    activeBattleId,
                    session.SessionId,
                    session.SessionGeneration);
            }
            arenaRuntime = new ArenaClientRuntime(
                tcpSender,
                reliableOutbound,
                inbound,
                battleLoad,
                session.SessionId,
                session.SessionGeneration,
                activeBattleId);
            arenaBinding = new ArenaInputBinding(
                arenaRuntime.Movement,
                arenaRuntime.Movement.ReadModel,
                arenaRuntime.Combat,
                arenaRuntime.Loot,
                arenaRuntime.Presentation,
                arenaRuntime.Input,
                session.SessionId);
            arenaRudpSubscription = router.Subscribe(
                (IRudpBindCapabilitySink)arenaRuntime.Movement);
            if (!arenaRuntime.IsTransportReady)
            {
                arenaBind = arenaRuntime.RequestTransportAsync(
                    ++nextTransportRequestId,
                    shutdown.Token);
            }
        }

        private async Task TickArenaAsync(CancellationToken cancellationToken)
        {
            ArenaClientRuntime current = arenaRuntime;
            if (current == null)
            {
                return;
            }
            await current.DrainAsync(cancellationToken);
            await current.TickAsync(
                (long)(Time.realtimeSinceStartupAsDouble * 1000.0),
                cancellationToken);
        }

        private void DrainMainThreadActions()
        {
            for (int index = 0; index < DispatcherBudget; index++)
            {
                Action action;
                lock (dispatchGate)
                {
                    if (pendingActions.Count == 0)
                    {
                        return;
                    }
                    action = pendingActions.Dequeue();
                }
                action();
            }
        }

        private void ObserveStartup()
        {
            if (startup == null || !startup.IsCompleted || startupFailureHandled)
            {
                return;
            }
            if (startup.IsFaulted || startup.IsCanceled)
            {
                startupFailureHandled = true;
                ShowLogin("로그인 또는 게임 연결을 완료하지 못했습니다.");
                Debug.LogWarning("Product player flow startup failed safely.");
            }
        }

        private void ObserveTransport()
        {
            Task tcpPump = transportLifetime?.TcpTask;
            if (tcpPump != null && tcpPump.IsCompleted && !shutdown.IsCancellationRequested &&
                !tcpFailureHandled)
            {
                tcpFailureHandled = true;
                if (CanResumeBattle())
                {
                    BeginReconnect();
                }
                else
                {
                    session?.Disconnect();
                    ShowLogin("게임 서버 연결이 종료되었습니다.");
                }
            }
            Task rudpPump = transportLifetime?.RudpTask;
            if (!shutdown.IsCancellationRequested && !rudpFailureHandled &&
                ((rudpPump != null && rudpPump.IsCompleted) ||
                 (rudpPump != null && reliableOutbound != null &&
                  reliableOutbound.HasConfirmedFailure)))
            {
                rudpFailureHandled = true;
                transportLifetime.ConfirmRudpFailure();
            }
        }

        private void ObserveArenaTasks()
        {
            ObserveCompleted(ref arenaTick);
            ObserveCompleted(ref arenaBind);
        }

        private void ObserveReconnect()
        {
            if (reconnect == null || !reconnect.IsCompleted)
            {
                return;
            }
            Task completed = reconnect;
            reconnect = null;
            if (completed.IsFaulted || completed.IsCanceled)
            {
#if DEVELOPMENT_BUILD || UNITY_EDITOR
                evidenceDriver?.OnReconnectFailed();
#endif
                RetireArenaAfterReconnectFailure();
                session?.Disconnect();
                HideBlockingFailure();
                ShowLogin("재연결에 실패해 로그인 화면으로 돌아갑니다.");
                return;
            }

            BattleSessionReconnectResult result =
                ((Task<BattleSessionReconnectResult>)completed).Result;
            if (result.Status != BattleSessionReconnectStatus.Resumed ||
                result.Snapshot == null)
            {
#if DEVELOPMENT_BUILD || UNITY_EDITOR
                evidenceDriver?.OnReconnectFailed();
#endif
                RetireArenaAfterReconnectFailure();
                session?.Disconnect();
                HideBlockingFailure();
                ShowLogin(result.Status == BattleSessionReconnectStatus.GraceExpired
                    ? "재접속 시간이 만료되었습니다."
                    : "재연결 인증에 실패했습니다.");
                return;
            }

#if DEVELOPMENT_BUILD || UNITY_EDITOR
            evidenceDriver?.OnSnapshotAcknowledged();
#endif

            if (!result.RequiresRudpRebind)
            {
                reconnectAwaitingRudp = false;
                battleLoad?.SetReconnectLocked(false);
                HideBlockingFailure();
                return;
            }
            if (!StartResumedArenaTransport(result.Snapshot))
            {
#if DEVELOPMENT_BUILD || UNITY_EDITOR
                evidenceDriver?.OnReconnectFailed();
#endif
                RetireArenaAfterReconnectFailure();
                session?.Disconnect();
                HideBlockingFailure();
                ShowLogin("재연결 인증에 실패했습니다.");
                return;
            }
            reconnectAwaitingRudp = true;
        }

        private void BeginReconnect()
        {
            if (reconnect != null)
            {
                return;
            }
#if DEVELOPMENT_BUILD || UNITY_EDITOR
            snapshotAppliedEvidenceReported = false;
#endif
            battleLoad.SetReconnectLocked(true);
#if DEVELOPMENT_BUILD || UNITY_EDITOR
            evidenceDriver?.OnReconnectStarted();
#endif
            ShowBlockingFailure("재연결 중");
            reconnect = ResumeBattleAsync();
        }

        private void HandleConfirmedTransportFailure()
        {
            if (CanResumeBattle())
            {
                BeginReconnect();
                return;
            }
            if (session != null && session.ConfirmRudpFailure())
            {
                shutdown.Cancel();
            }
        }

        private bool CanResumeBattle()
        {
            return reconnectClient != null && session != null &&
                session.State == PlayerSessionState.Authenticated &&
                battleLoad != null &&
                (battleLoad.IsWaiting || battleLoad.IsGameplayActive ||
                 battleResultHasFinalResult());
        }

        private bool ApplyResumeSnapshot(BattleResumeSnapshot snapshot)
        {
            if (snapshot == null || battleLoad == null ||
                battleResult == null || session == null ||
                session.State != PlayerSessionState.Authenticated)
            {
                return false;
            }
            if (snapshot.Phase != BattleResumePhase.Result ||
                arenaRuntime == null ||
                arenaRuntime.SessionGeneration != session.SessionGeneration)
            {
                if (!PrepareResumedArena(snapshot))
                {
                    return false;
                }
            }
            bool applied = new BattleSessionResumeApplier(
                    battleLoad,
                    battleResult,
                    arenaRuntime,
                    session)
                .Apply(snapshot);
            if (applied)
            {
#if DEVELOPMENT_BUILD || UNITY_EDITOR
                if (!snapshotAppliedEvidenceReported)
                {
                    snapshotAppliedEvidenceReported = true;
                    evidenceDriver?.OnSnapshotApplied();
                }
#endif
            }
            return applied;
        }

        private bool battleResultHasFinalResult()
        {
            return battleResult != null && battleResult.HasFinalResult;
        }

        private async Task<BattleSessionReconnectResult> ResumeBattleAsync()
        {
            return await reconnectClient.ResumeAsync(
                    session.SessionId,
                    session.SessionGeneration,
                    ConnectResumeTcpAsync,
                    shutdown.Token,
                    battleRecoveryProof)
                .ConfigureAwait(false);
        }

        private async Task<ITcpCommandSender> ConnectResumeTcpAsync(
            CancellationToken cancellationToken)
        {
            PlayerFlowTransportLifetime previous = transportLifetime;
            if (previous != null)
            {
                await previous.StopAsync().ConfigureAwait(false);
            }
            udp?.Close();
            tcp?.Close();
            tcp = new TcpClient(AddressFamily.InterNetwork);
            using (cancellationToken.Register(tcp.Close))
            {
                await tcp.ConnectAsync(
                    gameHost,
                    gameTcpPort);
            }
            NetworkStream stream = tcp.GetStream();
            ITcpCommandSender sender = new TcpCommandSender(stream);
            var nextLifetime = new PlayerFlowTransportLifetime(
                shutdown.Token,
                this,
                HandleConfirmedTransportFailure);
            nextLifetime.StartTcp(new TcpInboundPump(stream, router));
            transportLifetime = nextLifetime;
            BindTcpSender(sender);
            tcpFailureHandled = false;
            rudpFailureHandled = false;
            return sender;
        }

        private ITcpCommandSender BindTcpSender(ITcpCommandSender sender)
        {
            if (sender == null)
            {
                throw new ArgumentNullException(nameof(sender));
            }
            if (tcpSender is RebindableTcpCommandSender stable)
            {
                stable.Bind(sender);
                return stable;
            }
            tcpSender = new RebindableTcpCommandSender(sender);
            return tcpSender;
        }

        private bool PrepareResumedArena(BattleResumeSnapshot snapshot)
        {
            if (gameDatagramEndpoint == null || tcpSender == null ||
                battleLoad == null || session == null)
            {
                return false;
            }
            arenaRudpSubscription?.Dispose();
            arenaRudpSubscription = null;
            uint nextMovementActionSequence = arenaRuntime == null
                ? 1
                : arenaRuntime.Movement.NextActionSequence;
            udp = new UdpClient(AddressFamily.InterNetwork);
            udp.Connect(gameDatagramEndpoint);
            inbound = new RudpInboundPump(
                udp,
                gameDatagramEndpoint,
                session.SessionId,
                session.SessionGeneration);
            reliableOutbound = new RudpReliableOutbound(
                new UdpDatagramSender(udp),
                inbound);
            arenaRuntime = new ArenaClientRuntime(
                tcpSender,
                reliableOutbound,
                inbound,
                battleLoad,
                session.SessionId,
                session.SessionGeneration,
                snapshot.BattleInstanceId,
                nextMovementActionSequence);
            arenaBinding = new ArenaInputBinding(
                arenaRuntime.Movement,
                arenaRuntime.Movement.ReadModel,
                arenaRuntime.Combat,
                arenaRuntime.Loot,
                arenaRuntime.Presentation,
                arenaRuntime.Input,
                session.SessionId);
            arenaRudpSubscription = router.Subscribe(
                (IRudpBindCapabilitySink)arenaRuntime.Movement);
            return true;
        }

        private bool StartResumedArenaTransport(BattleResumeSnapshot snapshot)
        {
            if (arenaRuntime == null || inbound == null || transportLifetime == null ||
                snapshot == null || snapshot.Phase == BattleResumePhase.Result)
            {
                return false;
            }
            transportLifetime.StartRudp(inbound);
            arenaBind = arenaRuntime.RequestTransportAsync(
                ++nextTransportRequestId,
                shutdown.Token);
            return true;
        }

        private void RetireArenaAfterReconnectFailure()
        {
            if (transportLifetime != null)
            {
                _ = StopTransportSafelyAsync(transportLifetime);
            }
            udp?.Close();
            tcp?.Close();
            arenaRudpSubscription?.Dispose();
            arenaRudpSubscription = null;
            arenaBinding = null;
            arenaRuntime = null;
            reconnectAwaitingRudp = false;
            battleRecoveryProof = null;
            battleLoad?.ResetForLobby();
        }

        private static async Task StopTransportSafelyAsync(
            PlayerFlowTransportLifetime lifetime)
        {
            try
            {
                await lifetime.StopAsync();
            }
            catch (Exception)
            {
                Debug.LogWarning("Player transport shutdown failed safely.");
            }
        }

        private static void ObserveCompleted(ref Task task)
        {
            if (task == null || !task.IsCompleted)
            {
                return;
            }
            if (task.IsFaulted)
            {
                _ = task.Exception;
                Debug.LogWarning("Arena transport request failed safely.");
            }
            task = null;
        }

        private sealed class RebindableTcpCommandSender : ITcpCommandSender
        {
            private ITcpCommandSender target;

            public RebindableTcpCommandSender(ITcpCommandSender target)
            {
                Bind(target);
            }

            public void Bind(ITcpCommandSender next)
            {
                if (next == null)
                {
                    throw new ArgumentNullException(nameof(next));
                }
                Volatile.Write(ref target, next);
            }

            public Task SendAsync(
                byte[] frame,
                CancellationToken cancellationToken)
            {
                ITcpCommandSender current = Volatile.Read(ref target);
                if (current == null)
                {
                    throw new InvalidOperationException(
                        "TCP command sender is not bound");
                }
                return current.SendAsync(frame, cancellationToken);
            }
        }

        private static async Task<IPAddress> ResolveAddressAsync(
            string host,
            CancellationToken cancellationToken)
        {
            if (IPAddress.TryParse(host, out IPAddress address) &&
                address.AddressFamily == AddressFamily.InterNetwork)
            {
                return address;
            }
            cancellationToken.ThrowIfCancellationRequested();
            IPAddress[] addresses = await Dns.GetHostAddressesAsync(host);
            cancellationToken.ThrowIfCancellationRequested();
            IPAddress resolved = addresses.FirstOrDefault(
                candidate => candidate.AddressFamily == AddressFamily.InterNetwork);
            if (resolved == null)
            {
                throw new InvalidOperationException("Game host has no IPv4 address");
            }
            return resolved;
        }

        private static void ShowLogin(string copy)
        {
            LoginStatusTextView view =
                UnityEngine.Object.FindFirstObjectByType<LoginStatusTextView>();
            if (view != null)
            {
                view.ShowStatus(copy);
            }
        }

        private static void ShowBlockingFailure(string copy)
        {
            SafeFailureTextView view =
                UnityEngine.Object.FindFirstObjectByType<SafeFailureTextView>();
            view?.ShowBlockingMessage(copy);
        }

        private static void HideBlockingFailure()
        {
            SafeFailureTextView view =
                UnityEngine.Object.FindFirstObjectByType<SafeFailureTextView>();
            view?.HideBlockingMessage();
        }

        private void OnDestroy()
        {
            if (shutdown == null)
            {
                return;
            }
            session?.BeginClosing();
            shutdown.Cancel();
            if (transportLifetime != null)
            {
                _ = StopTransportSafelyAsync(transportLifetime);
            }
            if (battleLoad != null)
            {
                battleLoad.Changed -= EnsureArenaRuntime;
            }
            presentation?.Dispose();
            arenaRudpSubscription?.Dispose();
            arenaRudpSubscription = null;
            for (int index = subscriptions.Count - 1; index >= 0; index--)
            {
                subscriptions[index].Dispose();
            }
            subscriptions.Clear();
            udp?.Close();
            tcp?.Close();
            http?.Dispose();
            lock (dispatchGate)
            {
                pendingActions.Clear();
            }
        }

        private sealed class ProductConfiguration
        {
            private ProductConfiguration(
                Uri metaBaseUri,
                string gameHost,
                int tcpPort,
                int udpPort,
                string evidenceRole)
                : this(
                    metaBaseUri,
                    gameHost,
                    tcpPort,
                    udpPort,
                    evidenceRole,
                    false,
                    null)
            {
            }

            private ProductConfiguration(
                Uri metaBaseUri,
                string gameHost,
                int tcpPort,
                int udpPort,
                string evidenceRole,
                bool crashContinuityEvidence,
                MetaSessionIssued developmentMetaSession)
            {
                MetaBaseUri = metaBaseUri;
                GameHost = gameHost;
                TcpPort = tcpPort;
                UdpPort = udpPort;
                EvidenceRole = evidenceRole;
                IsCrashContinuityEvidence = crashContinuityEvidence;
                DevelopmentMetaSession = developmentMetaSession;
            }

            private ProductConfiguration(
                Uri metaBaseUri,
                string gameHost,
                int tcpPort,
                int udpPort,
                string evidenceRole,
                MetaSessionIssued developmentMetaSession)
                : this(
                    metaBaseUri,
                    gameHost,
                    tcpPort,
                    udpPort,
                    evidenceRole,
                    false,
                    developmentMetaSession)
            {
            }

            public Uri MetaBaseUri { get; }
            public string GameHost { get; }
            public int TcpPort { get; }
            public int UdpPort { get; }
            public string EvidenceRole { get; }
            public bool IsCrashContinuityEvidence { get; }
            public MetaSessionIssued DevelopmentMetaSession { get; }
            public bool IsDevelopmentEvidence =>
                !string.IsNullOrEmpty(EvidenceRole);
            public bool IsAutomatedEvidence =>
                EvidenceRole == "host" || EvidenceRole == "join";

            public static bool TryRead(
                IEnumerable<string> arguments,
                out ProductConfiguration configuration)
            {
                string meta = Value(arguments, "--loot-meta-base=");
                string host = Value(arguments, "--loot-game-host=");
                string tcp = Value(arguments, "--loot-tcp-port=");
                string udp = Value(arguments, "--loot-udp-port=");
                string evidence = Value(arguments, "--loot-e2e-role=");
                const string crashContinuityFlag =
                    "--loot-crash-continuity-evidence";
                bool hasCrashContinuityFlag = arguments.Any(candidate =>
                    candidate == crashContinuityFlag);
                bool hasMalformedCrashContinuityFlag = arguments.Any(candidate =>
                    candidate.StartsWith(
                        crashContinuityFlag,
                        StringComparison.Ordinal) &&
                    candidate != crashContinuityFlag);
                const string sessionFilePrefix = "--loot-meta-session-file=";
                bool hasSessionFile = arguments.Any(candidate => candidate.StartsWith(
                    sessionFilePrefix,
                    StringComparison.Ordinal));
                if (!Uri.TryCreate(meta, UriKind.Absolute, out Uri metaBaseUri) ||
                    string.IsNullOrWhiteSpace(host) ||
                    !TryPort(tcp, out int tcpPort) ||
                    !TryPort(udp, out int udpPort) ||
                    (!string.IsNullOrEmpty(evidence) &&
                     evidence != "host" && evidence != "join" &&
                     evidence != "manual"))
                {
                    configuration = null;
                    return false;
                }
#if DEVELOPMENT_BUILD || UNITY_EDITOR
                if (hasMalformedCrashContinuityFlag ||
                    (hasCrashContinuityFlag &&
                     evidence != "host" && evidence != "join"))
                {
                    configuration = null;
                    return false;
                }
#else
                if (hasCrashContinuityFlag || hasMalformedCrashContinuityFlag)
                {
                    configuration = null;
                    return false;
                }
#endif
#if DEVELOPMENT_BUILD || UNITY_EDITOR
                MetaSessionIssued developmentMetaSession = null;
                if (hasSessionFile &&
                    (evidence != "host" && evidence != "join" ||
                     !IsHttpLoopback(metaBaseUri) ||
                     !DevelopmentMetaSessionFile.TryRead(
                         Value(arguments, sessionFilePrefix),
                         out developmentMetaSession)))
                {
                    configuration = null;
                    return false;
                }
#else
                if (hasSessionFile)
                {
                    configuration = null;
                    return false;
                }
#endif
                configuration = new ProductConfiguration(
                    metaBaseUri,
                    host,
                    tcpPort,
                    udpPort,
                    evidence,
                    hasCrashContinuityFlag,
#if DEVELOPMENT_BUILD || UNITY_EDITOR
                    developmentMetaSession);
#else
                    null);
#endif
                return true;
            }

            private static bool IsHttpLoopback(Uri uri)
            {
                return uri != null && uri.IsAbsoluteUri &&
                    uri.Scheme == Uri.UriSchemeHttp &&
                    uri.Host == "127.0.0.1" &&
                    string.IsNullOrEmpty(uri.UserInfo) &&
                    string.IsNullOrEmpty(uri.Query) &&
                    string.IsNullOrEmpty(uri.Fragment);
            }

            private static string Value(
                IEnumerable<string> arguments,
                string prefix)
            {
                string argument = arguments.FirstOrDefault(
                    candidate => candidate.StartsWith(
                        prefix,
                        StringComparison.Ordinal));
                return argument == null ? null : argument.Substring(prefix.Length);
            }

            private static bool TryPort(string text, out int port)
            {
                return int.TryParse(text, out port) && port > 0 && port <= 65535;
            }
        }
    }

#if DEVELOPMENT_BUILD || UNITY_EDITOR
    internal static class DevelopmentMetaSessionFile
    {
        private const int MaximumBytes = 4096;
        private const uint RegularFileType = 0x8000;
        private const uint PermissionMask = 0x0fff;
        private const uint OwnerReadWrite = 0x0180;
        private static readonly UTF8Encoding StrictUtf8 =
            new UTF8Encoding(false, true);
        private static readonly string[] Rfc3339Formats =
        {
            "yyyy-MM-dd'T'HH:mm:ss'Z'",
            "yyyy-MM-dd'T'HH:mm:ss.FFFFFFF'Z'",
            "yyyy-MM-dd'T'HH:mm:sszzz",
            "yyyy-MM-dd'T'HH:mm:ss.FFFFFFFzzz"
        };

        public static bool TryRead(string path, out MetaSessionIssued issued)
        {
            issued = null;
            try
            {
                if (!Path.IsPathRooted(path) ||
                    !TryReadFileMetadata(path, out FileMetadata before) ||
                    !before.IsSecureRegularFile ||
                    before.Size <= 0 || before.Size > MaximumBytes)
                {
                    return false;
                }

                byte[] bytes = File.ReadAllBytes(path);
                if (bytes.Length == 0 || bytes.Length > MaximumBytes ||
                    !TryReadFileMetadata(path, out FileMetadata after) ||
                    !before.IsSameFile(after) || !after.IsSecureRegularFile)
                {
                    return false;
                }

                string json = StrictUtf8.GetString(bytes);
                if (!TryParsePayload(
                        json,
                        out string metaSession,
                        out string expiresAt) ||
                    !DateTimeOffset.TryParseExact(
                        expiresAt,
                        Rfc3339Formats,
                        CultureInfo.InvariantCulture,
                        DateTimeStyles.AssumeUniversal |
                            DateTimeStyles.AdjustToUniversal,
                        out DateTimeOffset expiration) ||
                    expiration <= DateTimeOffset.UtcNow)
                {
                    return false;
                }

                issued = new MetaSessionIssued(metaSession, expiration);
                return true;
            }
            catch (Exception)
            {
                issued = null;
                return false;
            }
        }

        private struct FileMetadata
        {
            public ulong Device;
            public ulong Inode;
            public uint Mode;
            public uint UserId;
            public long Size;

            public bool IsSecureRegularFile
            {
                get
                {
                    return (Mode & 0xf000) == RegularFileType &&
                        (Mode & PermissionMask) == OwnerReadWrite &&
                        UserId == CurrentUserId;
                }
            }

            public bool IsSameFile(FileMetadata other)
            {
                return Device == other.Device && Inode == other.Inode;
            }
        }

        private static bool TryReadFileMetadata(
            string path,
            out FileMetadata metadata)
        {
            metadata = default(FileMetadata);
#if UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX
            return TryReadDarwinFileMetadata(path, out metadata);
#elif UNITY_EDITOR_LINUX || UNITY_STANDALONE_LINUX
            return TryReadLinuxFileMetadata(path, out metadata);
#else
            return false;
#endif
        }

#if UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX
        private static bool TryReadDarwinFileMetadata(
            string path,
            out FileMetadata metadata)
        {
            metadata = default(FileMetadata);
            IntPtr status = IntPtr.Zero;
            try
            {
                status = Marshal.AllocHGlobal(256);
                if (LStat(path, status) != 0)
                {
                    return false;
                }
                metadata = new FileMetadata
                {
                    Device = unchecked((ulong)(uint)Marshal.ReadInt32(status, 0)),
                    Inode = unchecked((ulong)Marshal.ReadInt64(status, 8)),
                    Mode = unchecked((uint)(ushort)Marshal.ReadInt16(status, 4)),
                    UserId = unchecked((uint)Marshal.ReadInt32(status, 16)),
                    Size = Marshal.ReadInt64(status, 96)
                };
                return true;
            }
            catch (Exception)
            {
                return false;
            }
            finally
            {
                if (status != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(status);
                }
            }
        }
#elif UNITY_EDITOR_LINUX || UNITY_STANDALONE_LINUX
        private static bool TryReadLinuxFileMetadata(
            string path,
            out FileMetadata metadata)
        {
            metadata = default(FileMetadata);
            IntPtr status = IntPtr.Zero;
            try
            {
                status = Marshal.AllocHGlobal(256);
                if (LStat(path, status) != 0)
                {
                    return false;
                }
                metadata = new FileMetadata
                {
                    Device = unchecked((ulong)Marshal.ReadInt64(status, 0)),
                    Inode = unchecked((ulong)Marshal.ReadInt64(status, 8)),
                    Mode = unchecked((uint)Marshal.ReadInt32(status, 24)),
                    UserId = unchecked((uint)Marshal.ReadInt32(status, 28)),
                    Size = Marshal.ReadInt64(status, 48)
                };
                return true;
            }
            catch (Exception)
            {
                return false;
            }
            finally
            {
                if (status != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(status);
                }
            }
        }
#endif

#if UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX || UNITY_EDITOR_LINUX || UNITY_STANDALONE_LINUX
        private static uint CurrentUserId
        {
            get { return Geteuid(); }
        }

        [DllImport("libc", EntryPoint = "lstat", CallingConvention = CallingConvention.Cdecl)]
        private static extern int LStat(string path, IntPtr status);

        [DllImport("libc", EntryPoint = "geteuid", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint Geteuid();
#else
        private static uint CurrentUserId
        {
            get { return uint.MaxValue; }
        }
#endif

        private static bool TryParsePayload(
            string json,
            out string metaSession,
            out string expiresAt)
        {
            metaSession = null;
            expiresAt = null;
            int index = 0;
            SkipWhitespace(json, ref index);
            if (index >= json.Length || json[index++] != '{')
            {
                return false;
            }

            bool metaSeen = false;
            bool expiresSeen = false;
            while (true)
            {
                SkipWhitespace(json, ref index);
                if (index >= json.Length || json[index] == '}')
                {
                    return false;
                }
                if (!TryReadJsonString(json, ref index, out string name))
                {
                    return false;
                }
                SkipWhitespace(json, ref index);
                if (index >= json.Length || json[index++] != ':' ||
                    !TryReadJsonString(json, ref index, out string value))
                {
                    return false;
                }
                if (name == "metaSession")
                {
                    if (metaSeen)
                    {
                        return false;
                    }
                    metaSeen = true;
                    metaSession = value;
                }
                else if (name == "expiresAt")
                {
                    if (expiresSeen)
                    {
                        return false;
                    }
                    expiresSeen = true;
                    expiresAt = value;
                }
                else
                {
                    return false;
                }

                SkipWhitespace(json, ref index);
                if (index >= json.Length)
                {
                    return false;
                }
                if (json[index] == '}')
                {
                    index++;
                    break;
                }
                if (json[index++] != ',')
                {
                    return false;
                }
            }

            SkipWhitespace(json, ref index);
            return index == json.Length && metaSeen && expiresSeen;
        }

        private static bool TryReadJsonString(
            string json,
            ref int index,
            out string value)
        {
            value = null;
            if (index >= json.Length || json[index++] != '"')
            {
                return false;
            }
            var builder = new StringBuilder();
            while (index < json.Length)
            {
                char character = json[index++];
                if (character == '"')
                {
                    value = builder.ToString();
                    return true;
                }
                if (character < 0x20)
                {
                    return false;
                }
                if (character != '\\')
                {
                    builder.Append(character);
                    continue;
                }
                if (index >= json.Length)
                {
                    return false;
                }
                char escaped = json[index++];
                switch (escaped)
                {
                    case '"':
                    case '\\':
                    case '/':
                        builder.Append(escaped);
                        break;
                    case 'b':
                        builder.Append('\b');
                        break;
                    case 'f':
                        builder.Append('\f');
                        break;
                    case 'n':
                        builder.Append('\n');
                        break;
                    case 'r':
                        builder.Append('\r');
                        break;
                    case 't':
                        builder.Append('\t');
                        break;
                    case 'u':
                        if (index + 4 > json.Length ||
                            !TryReadHexCharacter(json, ref index, out int code))
                        {
                            return false;
                        }
                        builder.Append((char)code);
                        break;
                    default:
                        return false;
                }
            }
            return false;
        }

        private static bool TryReadHexCharacter(
            string json,
            ref int index,
            out int value)
        {
            value = 0;
            for (int offset = 0; offset < 4; offset++)
            {
                char character = json[index++];
                int digit;
                if (character >= '0' && character <= '9')
                {
                    digit = character - '0';
                }
                else if (character >= 'a' && character <= 'f')
                {
                    digit = character - 'a' + 10;
                }
                else if (character >= 'A' && character <= 'F')
                {
                    digit = character - 'A' + 10;
                }
                else
                {
                    return false;
                }
                value = (value << 4) | digit;
            }
            return true;
        }

        private static void SkipWhitespace(string json, ref int index)
        {
            while (index < json.Length &&
                   (json[index] == ' ' || json[index] == '\t' ||
                    json[index] == '\r' || json[index] == '\n'))
            {
                index++;
            }
        }
    }
#endif
}
