using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation;
using LootOfLegends.Presentation.Login;
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
        private PlayerFlowTransportLifetime transportLifetime;
        private RudpReliableOutbound reliableOutbound;
        private PlayerSessionReadModel session;
        private BattleLoadReadModel battleLoad;
        private ArenaClientRuntime arenaRuntime;
        private ArenaInputBinding arenaBinding;
        private TypedServerEventRouter router;
        private ITcpCommandSender tcpSender;
        private RudpInboundPump inbound;
        private PlayerFlowPresentationRuntime presentation;
#if DEVELOPMENT_BUILD || UNITY_EDITOR
        private DevelopmentPlayerFlowDriver evidenceDriver;
#endif
        private ulong nextTransportRequestId = 1000000;
        private ulong activeBattleId;
        private bool startupFailureHandled;
        private bool tcpFailureHandled;
        private bool rudpFailureHandled;

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
            ObserveTransport();
            ObserveArenaTasks();

            if (arenaRuntime != null && arenaTick == null)
            {
                arenaTick = TickArenaAsync(shutdown.Token);
            }
            presentation?.Tick();
#if DEVELOPMENT_BUILD || UNITY_EDITOR
            evidenceDriver?.Tick();
#endif
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
            ShowLogin("브라우저 로그인을 기다리고 있습니다.");
            await login.SignInAsync(cancellationToken);

            IssuedGameCredential credential = await new GameCredentialHttpApi(
                    http,
                    configuration.MetaBaseUri,
                    metaSession.Authorize)
                .IssueAsync(cancellationToken);

            tcp = new TcpClient(AddressFamily.InterNetwork);
            using (cancellationToken.Register(tcp.Close))
            {
                await tcp.ConnectAsync(
                    configuration.GameHost,
                    configuration.TcpPort);
            }

            session = new PlayerSessionReadModel();
            var lobbyRoom = new LobbyRoomReadModel();
            battleLoad = new BattleLoadReadModel();
            var battleResult = new BattleResultReadModel();
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
            tcpSender = new TcpCommandSender(stream);
            transportLifetime = new PlayerFlowTransportLifetime(
                cancellationToken,
                this,
                () =>
                {
                    if (session.ConfirmRudpFailure())
                    {
                        shutdown.Cancel();
                    }
                });
            transportLifetime.StartTcp(new TcpInboundPump(stream, router));
            await new GameSessionAuthenticator(tcpSender, session)
                .AuthenticateAsync(credential.Credential, cancellationToken);

            IPAddress gameAddress = await ResolveAddressAsync(
                configuration.GameHost,
                cancellationToken);
            var gameDatagramEndpoint = new IPEndPoint(
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
#if DEVELOPMENT_BUILD || UNITY_EDITOR
            if (configuration.IsDevelopmentEvidence)
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
                    cancellationToken);
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

            RemoveArenaSubscription();
            activeBattleId = battleLoad.BattleInstanceId;
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
                arenaRuntime.Input);
            subscriptions.Add(router.Subscribe(
                (IRudpBindCapabilitySink)arenaRuntime.Movement));
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
                session?.Disconnect();
                ShowLogin("게임 서버 연결이 종료되었습니다.");
            }
            Task rudpPump = transportLifetime?.RudpTask;
            if (!shutdown.IsCancellationRequested && !rudpFailureHandled &&
                ((rudpPump != null && rudpPump.IsCompleted) ||
                 (reliableOutbound != null &&
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

        private static void ObserveCompleted(ref Task task)
        {
            if (task == null || !task.IsCompleted)
            {
                return;
            }
            if (task.IsFaulted)
            {
                Debug.LogWarning("Arena transport request failed safely.");
            }
            task = null;
        }

        private void RemoveArenaSubscription()
        {
            if (subscriptions.Count == 0 || arenaRuntime == null)
            {
                return;
            }
            IDisposable subscription = subscriptions[subscriptions.Count - 1];
            subscriptions.RemoveAt(subscriptions.Count - 1);
            subscription.Dispose();
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
                _ = transportLifetime.StopAsync();
            }
            if (battleLoad != null)
            {
                battleLoad.Changed -= EnsureArenaRuntime;
            }
            presentation?.Dispose();
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
            {
                MetaBaseUri = metaBaseUri;
                GameHost = gameHost;
                TcpPort = tcpPort;
                UdpPort = udpPort;
                EvidenceRole = evidenceRole;
            }

            public Uri MetaBaseUri { get; }
            public string GameHost { get; }
            public int TcpPort { get; }
            public int UdpPort { get; }
            public string EvidenceRole { get; }
            public bool IsDevelopmentEvidence =>
                !string.IsNullOrEmpty(EvidenceRole);

            public static bool TryRead(
                IEnumerable<string> arguments,
                out ProductConfiguration configuration)
            {
                string meta = Value(arguments, "--loot-meta-base=");
                string host = Value(arguments, "--loot-game-host=");
                string tcp = Value(arguments, "--loot-tcp-port=");
                string udp = Value(arguments, "--loot-udp-port=");
                string evidence = Value(arguments, "--loot-e2e-role=");
                if (!Uri.TryCreate(meta, UriKind.Absolute, out Uri metaBaseUri) ||
                    string.IsNullOrWhiteSpace(host) ||
                    !TryPort(tcp, out int tcpPort) ||
                    !TryPort(udp, out int udpPort) ||
                    (!string.IsNullOrEmpty(evidence) &&
                     evidence != "host" && evidence != "join"))
                {
                    configuration = null;
                    return false;
                }
                configuration = new ProductConfiguration(
                    metaBaseUri,
                    host,
                    tcpPort,
                    udpPort,
                    evidence);
                return true;
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
}
