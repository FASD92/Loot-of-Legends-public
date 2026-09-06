using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Bootstrap;
using LootOfLegends.Presentation.Login;
using LootOfLegends.Presentation;
using LootOfLegends.Protocol;
using LootOfLegends.Session;
using LootOfLegends.Transport;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

namespace LootOfLegends.Tests.EditMode
{
    public sealed class StandaloneProductEntryTests
    {
        private const string MetaSession =
            "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";
        private const string GameCredential =
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

        [Test]
        public void BuildScenesStartAtLoginAndContainTheNormalPlayerJourney()
        {
            string[] scenes = Array.ConvertAll(
                EditorBuildSettings.scenes,
                scene => scene.path);

            Assert.That(scenes, Is.EqualTo(new[]
            {
                "Assets/Scenes/LoginScene.unity",
                "Assets/Scenes/LobbyScene.unity",
                "Assets/Scenes/RoomScene.unity",
                "Assets/Scenes/ArenaScene.unity"
            }));

            Scene login = EditorSceneManager.OpenScene(
                scenes[0], OpenSceneMode.Additive);
            try
            {
                Assert.That(
                    UnityEngine.Object.FindFirstObjectByType<LoginStatusTextView>(),
                    Is.Not.Null);
            }
            finally
            {
                EditorSceneManager.CloseScene(login, true);
            }
        }

        [Test]
        public void ManualEvidenceRoleUsesFixtureAuthWithoutAutomation()
        {
            Type configurationType = typeof(ProductPlayerFlowBootstrap).GetNestedType(
                "ProductConfiguration",
                BindingFlags.Static | BindingFlags.NonPublic);
            MethodInfo tryRead = configurationType.GetMethod(
                "TryRead",
                BindingFlags.Static | BindingFlags.Public);
            object[] arguments =
            {
                new[]
                {
                    "--loot-meta-base=http://127.0.0.1:8080",
                    "--loot-game-host=127.0.0.1",
                    "--loot-tcp-port=40000",
                    "--loot-udp-port=40000",
                    "--loot-e2e-role=manual"
                },
                null
            };

            bool accepted = (bool)tryRead.Invoke(null, arguments);
            PropertyInfo automated = configurationType.GetProperty(
                "IsAutomatedEvidence",
                BindingFlags.Instance | BindingFlags.Public);

            Assert.That(accepted, Is.True);
            Assert.That(automated, Is.Not.Null);
            Assert.That(
                automated.GetValue(arguments[1]),
                Is.EqualTo(false));
        }

        [Test]
        public void CrashContinuityFlagEnablesCrashEvidenceForHost()
        {
            object configuration;
            Assert.That(
                TryReadConfiguration(
                    new[]
                    {
                        "--loot-meta-base=http://127.0.0.1:8080",
                        "--loot-game-host=127.0.0.1",
                        "--loot-tcp-port=40000",
                        "--loot-udp-port=40000",
                        "--loot-e2e-role=host",
                        "--loot-crash-continuity-evidence"
                    },
                    out configuration),
                Is.True);
            PropertyInfo crashEvidence = configuration.GetType().GetProperty(
                "IsCrashContinuityEvidence",
                BindingFlags.Instance | BindingFlags.Public);
            Assert.That(crashEvidence, Is.Not.Null);
            Assert.That(crashEvidence.GetValue(configuration), Is.EqualTo(true));
        }

        [Test]
        public void CrashContinuityFlagRejectsManualEvidenceRole()
        {
            object configuration;
            Assert.That(
                TryReadConfiguration(
                    new[]
                    {
                        "--loot-meta-base=http://127.0.0.1:8080",
                        "--loot-game-host=127.0.0.1",
                        "--loot-tcp-port=40000",
                        "--loot-udp-port=40000",
                        "--loot-e2e-role=manual",
                        "--loot-crash-continuity-evidence"
                    },
                    out configuration),
                Is.False);
        }

        [Test]
        public void CrashContinuityFlagRejectsValueSuffix()
        {
            object configuration;
            Assert.That(
                TryReadConfiguration(
                    new[]
                    {
                        "--loot-meta-base=http://127.0.0.1:8080",
                        "--loot-game-host=127.0.0.1",
                        "--loot-tcp-port=40000",
                        "--loot-udp-port=40000",
                        "--loot-e2e-role=host",
                        "--loot-crash-continuity-evidence=true"
                    },
                    out configuration),
                Is.False);
        }

        [Test]
        public void DevelopmentMetaSessionFileAcceptsSecureLoopbackHost()
        {
            using (var file = new TemporaryMetaSessionFile(
                       "{\"metaSession\":\"" + MetaSession +
                       "\",\"expiresAt\":\"2099-01-01T00:00:00Z\"}"))
            {
                object configuration;
                Assert.That(
                    TryReadConfiguration(
                        new[]
                        {
                            "--loot-meta-base=http://127.0.0.1:8080",
                            "--loot-game-host=127.0.0.1",
                            "--loot-tcp-port=40000",
                            "--loot-udp-port=40000",
                            "--loot-e2e-role=host",
                            "--loot-meta-session-file=" + file.Path
                        },
                        out configuration),
                    Is.True);
                PropertyInfo issuedProperty = configuration.GetType().GetProperty(
                    "DevelopmentMetaSession",
                    BindingFlags.Instance | BindingFlags.Public);
                Assert.That(issuedProperty, Is.Not.Null);
                var issued = (MetaSessionIssued)issuedProperty.GetValue(configuration);
                Assert.That(issued, Is.Not.Null);
                Assert.That(issued.MetaSession, Is.EqualTo(MetaSession));
            }
        }

        [Test]
        public void DevelopmentMetaSessionFileRejectsNonLoopbackBase()
        {
            using (var file = new TemporaryMetaSessionFile(ValidMetaSessionJson()))
            {
                object configuration;
                Assert.That(
                    TryReadConfiguration(
                        SessionFileArguments("https://meta.example", file.Path),
                        out configuration),
                    Is.False);
            }
        }

        [Test]
        public void DevelopmentMetaSessionFileRejectsRelativePath()
        {
            object configuration;
            Assert.That(
                TryReadConfiguration(
                    SessionFileArguments(
                        "http://127.0.0.1:8080",
                        "relative-session.json"),
                    out configuration),
                Is.False);
        }

        [Test]
        public void DevelopmentMetaSessionFileRejectsPermissiveMode()
        {
            using (var file = new TemporaryMetaSessionFile(ValidMetaSessionJson()))
            {
                file.SetMode(Convert.ToUInt32("0644", 8));
                object configuration;
                Assert.That(
                    TryReadConfiguration(
                        SessionFileArguments(
                            "http://127.0.0.1:8080",
                            file.Path),
                        out configuration),
                    Is.False);
            }
        }

        [Test]
        public void DevelopmentMetaSessionFileRejectsExpiredValue()
        {
            using (var file = new TemporaryMetaSessionFile(
                       "{\"metaSession\":\"" + MetaSession +
                       "\",\"expiresAt\":\"2000-01-01T00:00:00Z\"}"))
            {
                object configuration;
                Assert.That(
                    TryReadConfiguration(
                        SessionFileArguments(
                            "http://127.0.0.1:8080",
                            file.Path),
                        out configuration),
                    Is.False);
            }
        }

        [Test]
        public void DevelopmentMetaSessionFileRejectsExtraField()
        {
            using (var file = new TemporaryMetaSessionFile(
                       "{\"metaSession\":\"" + MetaSession +
                       "\",\"expiresAt\":\"2099-01-01T00:00:00Z\"," +
                       "\"extra\":\"rejected\"}"))
            {
                object configuration;
                Assert.That(
                    TryReadConfiguration(
                        SessionFileArguments(
                            "http://127.0.0.1:8080",
                            file.Path),
                        out configuration),
                    Is.False);
            }
        }

        [Test]
        public async Task BootstrapBrowserAuthenticationWaitingCopyReachesLoginStatusView()
        {
            const string expected = "브라우저 인증 완료를 기다리고 있습니다.";

            Scene login = EditorSceneManager.OpenScene(
                "Assets/Scenes/LoginScene.unity", OpenSceneMode.Additive);
            var root = new GameObject("Bootstrap browser authentication copy test");
            try
            {
                var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                Type configurationType = typeof(ProductPlayerFlowBootstrap).GetNestedType(
                    "ProductConfiguration",
                    BindingFlags.Static | BindingFlags.NonPublic);
                Assert.That(configurationType, Is.Not.Null);
                object configuration = Activator.CreateInstance(
                    configurationType,
                    BindingFlags.Instance | BindingFlags.NonPublic,
                    null,
                    new object[]
                    {
                        new Uri("https://meta.invalid/"),
                        "127.0.0.1",
                        40000,
                        40000,
                        string.Empty
                    },
                    null);
                MethodInfo startFlow = typeof(ProductPlayerFlowBootstrap).GetMethod(
                    "StartFlowAsync",
                    BindingFlags.Instance | BindingFlags.NonPublic);
                Assert.That(startFlow, Is.Not.Null);

                Task startup;
                using (var cancellation = new CancellationTokenSource())
                {
                    cancellation.Cancel();
                    startup = (Task)startFlow.Invoke(
                        bootstrap,
                        new object[] { configuration, cancellation.Token });

                    LoginStatusTextView view =
                        UnityEngine.Object.FindFirstObjectByType<LoginStatusTextView>();
                    Assert.That(view, Is.Not.Null);
                    Text label = (Text)GetField(view, "label");
                    Assert.That(label.text, Is.EqualTo(expected));
                    Assert.CatchAsync<OperationCanceledException>(async () =>
                        await startup);
                }
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(root);
                EditorSceneManager.CloseScene(login, true);
            }
        }

        [Test]
        public void BootstrapAssemblyCanComposeEveryApprovedCapability()
        {
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Bootstrap/LootOfLegends.Bootstrap.asmdef");
            string assembly = File.ReadAllText(path);

            foreach (string dependency in new[]
                     {
                         "LootOfLegends.Transport",
                         "LootOfLegends.Session",
                         "LootOfLegends.LobbyRoom",
                         "LootOfLegends.Battle",
                         "LootOfLegends.Collection",
                         "LootOfLegends.Presentation"
                     })
            {
                StringAssert.Contains($"\"{dependency}\"", assembly);
            }
            StringAssert.DoesNotContain("\"noEngineReferences\": true", assembly);
            Assert.That(typeof(ProductPlayerFlowBootstrap), Is.Not.Null);
        }

        [Test]
        public void ProductBootstrapOwnsOneReliableOutboundAndOneTransportLifetime()
        {
            FieldInfo[] fields = typeof(ProductPlayerFlowBootstrap).GetFields(
                BindingFlags.Instance | BindingFlags.NonPublic);

            Assert.That(
                fields.Count(field => field.FieldType == typeof(RudpReliableOutbound)),
                Is.EqualTo(1));
            Assert.That(
                fields.Count(field => field.FieldType == typeof(PlayerFlowTransportLifetime)),
                Is.EqualTo(1));
            Assert.That(fields.Select(field => field.Name),
                Has.None.EqualTo("tcpPump"));
            Assert.That(fields.Select(field => field.Name),
                Has.None.EqualTo("rudpPump"));

            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Bootstrap/ProductPlayerFlowBootstrap.cs");
            string source = File.ReadAllText(path);
            StringAssert.Contains("new RudpReliableOutbound(", source);
            StringAssert.Contains("transportLifetime.StartTcp(", source);
            StringAssert.Contains("transportLifetime.StartRudp(", source);
            StringAssert.Contains("reliableOutbound.HasConfirmedFailure", source);
            StringAssert.Contains("transportLifetime.ConfirmRudpFailure()", source);
        }

        [Test]
        public async Task ProductBootstrapHandsReliableExpiryToTheSessionExactlyOnce()
        {
            var root = new GameObject("Reliable expiry product test");
            var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
            var session = new PlayerSessionReadModel();
            session.BeginAuthentication();
            session.Apply(new WelcomeSession(1, 2, 3, 0, "player-one"));
            var shutdown = new CancellationTokenSource();
            SetField(bootstrap, "shutdown", shutdown);
            var lifetime = new PlayerFlowTransportLifetime(
                shutdown.Token,
                bootstrap,
                () =>
                {
                    if (session.ConfirmRudpFailure())
                    {
                        shutdown.Cancel();
                    }
                });
            long now = 0;

            using (var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
            {
                var pump = new RudpInboundPump(
                    socket,
                    new IPEndPoint(IPAddress.Loopback, 7777),
                    1,
                    2);
                var reliable = new RudpReliableOutbound(
                    new RecordingRudpSender(), pump, () => now);
                SetField(bootstrap, "session", session);
                SetField(bootstrap, "transportLifetime", lifetime);
                SetField(bootstrap, "reliableOutbound", reliable);
                lifetime.StartRudp(pump);

                await reliable.SendBindHelloAsync(
                    Enumerable.Range(1, 32).Select(value => (byte)value).ToArray(),
                    CancellationToken.None);
                now = 5000;
                await reliable.TickAsync(CancellationToken.None);
                Invoke(bootstrap, "ObserveTransport");
                Invoke(bootstrap, "DrainMainThreadActions");
                Invoke(bootstrap, "ObserveTransport");

                Assert.That(session.LastFailure,
                    Is.EqualTo(PlayerSessionFailure.RudpUnavailable));
                Assert.That(session.State,
                    Is.EqualTo(PlayerSessionState.Disconnected));
            }

            await lifetime.StopAsync();
            UnityEngine.Object.DestroyImmediate(root);
        }

        [Test]
        public async Task PortfolioF8ClosesOnlyTheActiveBattleTcpConnection()
        {
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var root = new GameObject("Portfolio F8 disconnect test");
            TcpClient server = null;
            try
            {
                var bootstrap = root.AddComponent<ProductPlayerFlowBootstrap>();
                var client = new TcpClient(AddressFamily.InterNetwork);
                Task<TcpClient> accept = listener.AcceptTcpClientAsync();
                await client.ConnectAsync(
                    IPAddress.Loopback,
                    ((IPEndPoint)listener.LocalEndpoint).Port);
                server = await accept;
                NetworkStream stream = client.GetStream();

                var session = new PlayerSessionReadModel();
                session.BeginAuthentication();
                session.Apply(new WelcomeSession(1, 2, 3, 0, "player-one"));
                var load = new BattleLoadReadModel();
                var reconnectClient = new BattleSessionReconnectClient(
                    new CallbackSender(() => { }),
                    _ => Task.FromResult(GameCredential),
                    new StopwatchReconnectClock(),
                    _ => true,
                    bootstrap);
                SetField(bootstrap, "tcp", client);
                SetField(bootstrap, "session", session);
                SetField(bootstrap, "battleLoad", load);
                SetField(bootstrap, "reconnectClient", reconnectClient);

                Invoke(bootstrap, "HandlePortfolioDisconnectShortcut", true);
                Assert.DoesNotThrow(() => stream.WriteByte(1));

                Assert.That(load.Apply(new ArenaLoadEntry(7, 9)), Is.True);
                Assert.That(load.Apply(new ArenaGameplayStart(
                    7,
                    9,
                    new[] { new BattleParticipant(1, 2, "player-one") })), Is.True);
                Invoke(bootstrap, "HandlePortfolioDisconnectShortcut", true);

                Assert.That(() => stream.WriteByte(2), Throws.Exception);
                Assert.That(session.State, Is.EqualTo(PlayerSessionState.Authenticated));
                Assert.That(load.IsGameplayActive, Is.True);
            }
            finally
            {
                server?.Close();
                listener.Stop();
                UnityEngine.Object.DestroyImmediate(root);
            }
        }

        [Test]
        public async Task MetaSessionIssuesBoundedGameCredentialWithoutProviderToken()
        {
            var handler = new RecordingHandler(
                HttpStatusCode.Created,
                "{\"credential\":\"" + GameCredential +
                "\",\"expiresAt\":\"2099-01-01T00:00:00Z\"}");
            var session = new MetaSessionState();
            session.Accept(new MetaSessionIssued(
                MetaSession,
                DateTimeOffset.UtcNow.AddMinutes(5)));
            var api = new GameCredentialHttpApi(
                new HttpClient(handler),
                new Uri("http://127.0.0.1:18080/"),
                session.Authorize);

            IssuedGameCredential issued = await api.IssueAsync(
                CancellationToken.None);

            Assert.That(issued.Credential, Is.EqualTo(GameCredential));
            Assert.That(handler.RequestUri.AbsolutePath,
                Is.EqualTo("/api/v1/game-credentials"));
            Assert.That(handler.AuthorizationScheme, Is.EqualTo("Bearer"));
            Assert.That(handler.AuthorizationParameter, Is.EqualTo(MetaSession));
        }

        [Test]
        public async Task GameAuthenticationUsesTheSingleTcpPumpProjection()
        {
            var session = new PlayerSessionReadModel();
            var sender = new CallbackSender(() => session.Apply(
                new WelcomeSession(1, 10, 2, 0, "player-one")));
            var authenticator = new GameSessionAuthenticator(sender, session);

            await authenticator.AuthenticateAsync(
                GameCredential,
                CancellationToken.None);

            Assert.That(session.State, Is.EqualTo(PlayerSessionState.Authenticated));
            Assert.That(sender.SendCount, Is.EqualTo(1));
        }

        [Test]
        public void ArenaEntryBeginsExactFinalResultCorrelation()
        {
            var load = new BattleLoadReadModel();
            var result = new BattleResultReadModel();
            var router = new BattleCompletionRouter(
                new BattleResponseCorrelator(), load, result);

            router.OnMessage(new ArenaLoadEntry(7, 9));

            Assert.That(result.CurrentRoomId, Is.EqualTo(7));
            Assert.That(result.CurrentBattleInstanceId, Is.EqualTo(9));
        }

        [Test]
        public void ArenaLoadCompletionWaitsForRudpTransportReady()
        {
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/PlayerFlowPresentationRuntime.cs");
            string source = File.ReadAllText(path);

            StringAssert.Contains(
                "binding == null || !binding.IsTransportReady",
                source);
        }

        [Test]
        public void KeyboardMovementKeepsTheZeroStopIntentOnItsBoundedCadence()
        {
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/PlayerFlowControls.cs");
            string source = File.ReadAllText(path);

            StringAssert.Contains(
                "currentArena.Presentation.Snapshot().ControlsEnabled",
                source);
            StringAssert.Contains("if (now >= nextMoveAt)", source);
            StringAssert.DoesNotContain(
                "(x != 0 || y != 0) && now >= nextMoveAt",
                source);
        }

        [Test]
        public void EvidenceMovementRetriesUnreliableIntentUntilServerProjection()
        {
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/DevelopmentPlayerFlowDriver.cs");
            string source = File.ReadAllText(path);

            StringAssert.Contains("MovementRetryIntervalSeconds", source);
            StringAssert.Contains("nextMovementSubmitAt", source);
            StringAssert.Contains("if (now < nextMovementSubmitAt)", source);
            StringAssert.Contains(
                "nextMovementSubmitAt = now + MovementRetryIntervalSeconds",
                source);
        }

        [Test]
        public void KeyboardDiscreteArenaCommandsPrecedePeriodicMovement()
        {
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/PlayerFlowControls.cs");
            string source = File.ReadAllText(path);

            Assert.That(
                source.IndexOf(
                    "Input.GetKeyDown(KeyCode.Space)",
                    StringComparison.Ordinal),
                Is.LessThan(source.IndexOf(
                    "if (now >= nextMoveAt)",
                    StringComparison.Ordinal)));
            Assert.That(
                source.IndexOf(
                    "Input.GetKeyDown(KeyCode.E)",
                    StringComparison.Ordinal),
                Is.LessThan(source.IndexOf(
                    "if (now >= nextMoveAt)",
                    StringComparison.Ordinal)));
        }

        [Test]
        public void KeyboardDoesNotSubmitAttackAfterCombatTerminal()
        {
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/PlayerFlowControls.cs");
            string source = File.ReadAllText(path);

            StringAssert.Contains(
                "current.Presentation.Snapshot().CanAttack",
                source);
        }

        [Test]
        public void KeyboardInputOwnsOnlyArenaRoomMutationCommands()
        {
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/PlayerFlowControls.cs");
            string source = File.ReadAllText(path);
            int keyboardStart = source.IndexOf(
                "public sealed class PlayerFlowKeyboardInput",
                StringComparison.Ordinal);
            Assert.That(keyboardStart, Is.GreaterThanOrEqualTo(0));
            string keyboard = source.Substring(keyboardStart);

            StringAssert.Contains(
                "currentArena == null ||\n                SceneManager.GetActiveScene().name != \"ArenaScene\"",
                keyboard);
            StringAssert.DoesNotContain("TickRoom(", keyboard);
            StringAssert.DoesNotContain("Input.GetKeyDown(KeyCode.C)", keyboard);
            StringAssert.DoesNotContain("Input.GetKeyDown(KeyCode.J)", keyboard);
            StringAssert.DoesNotContain("Input.GetKeyDown(KeyCode.R)", keyboard);
            StringAssert.DoesNotContain("Input.GetKeyDown(KeyCode.S)", keyboard);
            StringAssert.DoesNotContain("roomCommands.CreateAsync", keyboard);
            StringAssert.DoesNotContain("roomCommands.JoinAsync", keyboard);
            StringAssert.DoesNotContain("roomCommands.SetReadyAsync", keyboard);
            StringAssert.DoesNotContain("hostStart.StartAsync", keyboard);
            StringAssert.DoesNotContain("private readonly LobbyRoomReadModel", keyboard);
            StringAssert.DoesNotContain("private readonly IRoomHostStartAction", keyboard);
        }

        [Test]
        public void TwoClientEvidenceDriverIsDevelopmentOnlyAndUsesCapabilityState()
        {
            Assert.That(typeof(DevelopmentPlayerFlowDriver), Is.Not.Null);
            string path = Path.Combine(
                Application.dataPath,
                "LootOfLegends/Presentation/DevelopmentPlayerFlowDriver.cs");
            string source = File.ReadAllText(path);

            StringAssert.Contains("#if DEVELOPMENT_BUILD || UNITY_EDITOR", source);
            StringAssert.Contains("FinalLootProjectionGraceSeconds", source);
            StringAssert.DoesNotContain("ProtocolCodec", source);
            StringAssert.DoesNotContain("TcpClient", source);
            StringAssert.DoesNotContain("UdpClient", source);
        }

        private sealed class RecordingHandler : HttpMessageHandler
        {
            private readonly HttpStatusCode status;
            private readonly string response;

            public RecordingHandler(HttpStatusCode status, string response)
            {
                this.status = status;
                this.response = response;
            }

            public Uri RequestUri { get; private set; }
            public string AuthorizationScheme { get; private set; }
            public string AuthorizationParameter { get; private set; }

            protected override Task<HttpResponseMessage> SendAsync(
                HttpRequestMessage request,
                CancellationToken cancellationToken)
            {
                RequestUri = request.RequestUri;
                AuthorizationScheme = request.Headers.Authorization?.Scheme;
                AuthorizationParameter = request.Headers.Authorization?.Parameter;
                return Task.FromResult(new HttpResponseMessage(status)
                {
                    Content = new StringContent(response, Encoding.UTF8, "application/json")
                });
            }
        }

        private static string ValidMetaSessionJson()
        {
            return "{\"metaSession\":\"" + MetaSession +
                "\",\"expiresAt\":\"2099-01-01T00:00:00Z\"}";
        }

        private static string[] SessionFileArguments(string metaBase, string path)
        {
            return new[]
            {
                "--loot-meta-base=" + metaBase,
                "--loot-game-host=127.0.0.1",
                "--loot-tcp-port=40000",
                "--loot-udp-port=40000",
                "--loot-e2e-role=host",
                "--loot-meta-session-file=" + path
            };
        }

        private static bool TryReadConfiguration(
            string[] arguments,
            out object configuration)
        {
            Type configurationType = typeof(ProductPlayerFlowBootstrap).GetNestedType(
                "ProductConfiguration",
                BindingFlags.Static | BindingFlags.NonPublic);
            MethodInfo tryRead = configurationType.GetMethod(
                "TryRead",
                BindingFlags.Static | BindingFlags.Public);
            object[] values = { arguments, null };
            bool accepted = (bool)tryRead.Invoke(null, values);
            configuration = values[1];
            return accepted;
        }

        private sealed class TemporaryMetaSessionFile : IDisposable
        {
            public TemporaryMetaSessionFile(string contents)
            {
                Path = System.IO.Path.Combine(
                    System.IO.Path.GetTempPath(),
                    "lol-meta-session-" + Guid.NewGuid().ToString("N") + ".json");
                File.WriteAllText(Path, contents, new UTF8Encoding(false));
#if UNITY_EDITOR_OSX || UNITY_EDITOR_LINUX
                if (chmod(Path, Convert.ToUInt32("0600", 8)) != 0)
                {
                    throw new InvalidOperationException(
                        "Unable to create a 0600 Meta session file");
                }
#else
                throw new InvalidOperationException(
                    "Secure Meta session file tests require Unix file modes");
#endif
            }

            public string Path { get; }

            public void SetMode(uint mode)
            {
#if UNITY_EDITOR_OSX || UNITY_EDITOR_LINUX
                if (chmod(Path, mode) != 0)
                {
                    throw new InvalidOperationException(
                        "Unable to set Meta session file mode");
                }
#else
                throw new InvalidOperationException(
                    "Secure Meta session file tests require Unix file modes");
#endif
            }

            public void Dispose()
            {
                if (File.Exists(Path))
                {
                    File.Delete(Path);
                }
            }

#if UNITY_EDITOR_OSX || UNITY_EDITOR_LINUX
            [DllImport("libc", EntryPoint = "chmod", SetLastError = true)]
            private static extern int chmod(string path, uint mode);
#endif
        }

        private sealed class CallbackSender : LootOfLegends.Transport.ITcpCommandSender
        {
            private readonly Action callback;

            public CallbackSender(Action callback)
            {
                this.callback = callback;
            }

            public int SendCount { get; private set; }

            public Task SendAsync(byte[] frame, CancellationToken cancellationToken)
            {
                SendCount++;
                callback();
                return Task.CompletedTask;
            }
        }

        private static object GetField(object target, string name)
        {
            FieldInfo field = target.GetType().GetField(
                name,
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null);
            return field.GetValue(target);
        }

        private static void SetField(object target, string name, object value)
        {
            FieldInfo field = target.GetType().GetField(
                name,
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null);
            field.SetValue(target, value);
        }

        private static void Invoke(object target, string name)
        {
            MethodInfo method = target.GetType().GetMethod(
                name,
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(method, Is.Not.Null);
            method.Invoke(target, Array.Empty<object>());
        }

        private static void Invoke(object target, string name, bool argument)
        {
            MethodInfo method = target.GetType().GetMethod(
                name,
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(method, Is.Not.Null);
            method.Invoke(target, new object[] { argument });
        }

        private sealed class RecordingRudpSender : IRudpDatagramSender
        {
            public Task SendAsync(
                byte[] datagram,
                CancellationToken cancellationToken)
            {
                return Task.CompletedTask;
            }
        }
    }
}
