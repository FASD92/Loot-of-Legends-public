using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Reflection;
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
