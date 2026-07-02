using LootOfLegends.PlayerClient;
using NUnit.Framework;
using System.Reflection;
using System.Threading.Tasks;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerLocalControllerTests
    {
        [Test]
        public void BuildIntentFromDigitalInputCancelsOpposingDirectionsAndKeepsSpaceLoot()
        {
            PlayerInputIntent intent = PlayerLocalController.BuildIntentFromDigitalInput(
                leftPressed: true,
                rightPressed: true,
                backwardPressed: false,
                forwardPressed: true,
                spaceLootPressed: true);

            Assert.AreEqual(0, intent.MoveX);
            Assert.AreEqual(1, intent.MoveZ);
            Assert.True(intent.SpaceLootPressed);
        }

        [Test]
        public void DefaultMovementSpeedMatchesServerAuthoritativeSpeed()
        {
            Assert.AreEqual(1.0f, PlayerLocalMovement.DefaultSpeedUnitsPerSecond);
        }

        [Test]
        public void DefaultArenaClampMatchesServerAuthoritativeArena()
        {
            Assert.AreEqual(50.0f, PlayerLocalMovement.DefaultArenaClampHalfExtent);
        }

        [Test]
        public void StepMovesTransformFromInputIntent()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                avatar.transform.localPosition = new Vector3(0.0f, 1.1f, 0.0f);
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                controller.SpeedUnitsPerSecond = 4.0f;
                controller.ArenaClampHalfExtent = 9.0f;

                controller.Step(PlayerInputIntent.FromKeyboard(1.0f, 0.0f, false), 0.5f);

                Assert.AreEqual(2.0f, avatar.transform.localPosition.x, 0.0001f);
                Assert.AreEqual(1.1f, avatar.transform.localPosition.y, 0.0001f);
                Assert.AreEqual(0.0f, avatar.transform.localPosition.z, 0.0001f);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public void StepDrivesVisualMovementIntentAndFacing()
        {
            Release0ResourcesVisualProvider provider = new Release0ResourcesVisualProvider();
            GameObject avatar = provider.CreateLocalPlayer();
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                controller.SpeedUnitsPerSecond = 4.0f;
                controller.ArenaClampHalfExtent = 9.0f;

                Release0PlayerVisualAnimator visualAnimator =
                    avatar.GetComponent<Release0PlayerVisualAnimator>();
                Animator animator = avatar.GetComponentInChildren<Animator>(true);
                Assert.NotNull(visualAnimator);
                Assert.NotNull(animator);

                Vector2Int[] directions =
                {
                    new Vector2Int(0, 1),
                    new Vector2Int(1, 1),
                    new Vector2Int(1, 0),
                    new Vector2Int(1, -1),
                    new Vector2Int(0, -1),
                    new Vector2Int(-1, -1),
                    new Vector2Int(-1, 0),
                    new Vector2Int(-1, 1)
                };

                foreach (Vector2Int direction in directions)
                {
                    controller.Step(
                        PlayerInputIntent.FromKeyboard(
                            direction.x,
                            direction.y,
                            false),
                        0.1f);

                    Vector3 expectedForward =
                        new Vector3(direction.x, 0.0f, direction.y).normalized;
                    Assert.IsTrue(
                        animator.GetBool(Release0PlayerVisualAnimator.IsMovingParameter),
                        $"Expected moving for {direction}");
                    Assert.Greater(
                        Vector3.Dot(animator.transform.forward, expectedForward),
                        0.99f,
                        $"Expected visual to face {direction}");
                }

                controller.Step(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false),
                    0.1f);

                Assert.IsFalse(
                    animator.GetBool(Release0PlayerVisualAnimator.IsMovingParameter));
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public void ApplyAuthoritativeSelfPositionUsesSelfStateSnapshot()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                avatar.transform.localPosition = new Vector3(9.0f, 1.1f, -9.0f);
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                PlayerRudpStateSnapshot snapshot = new PlayerRudpStateSnapshot(
                    17U,
                    9U,
                    new[]
                    {
                        new PlayerRudpStateSnapshotPlayer(11UL, -1000, 0),
                        new PlayerRudpStateSnapshotPlayer(22UL, 1250, -500)
                    });

                MethodInfo method = typeof(PlayerLocalController).GetMethod(
                    "ApplyAuthoritativeSelfPosition",
                    new[] { typeof(PlayerRudpStateSnapshot), typeof(ulong) });

                Assert.NotNull(method);
                bool applied = (bool)method.Invoke(
                    controller,
                    new object[] { snapshot, 22UL });

                Assert.IsTrue(applied);
                Assert.AreEqual(1.25f, avatar.transform.localPosition.x, 0.0001f);
                Assert.AreEqual(1.1f, avatar.transform.localPosition.y, 0.0001f);
                Assert.AreEqual(-0.5f, avatar.transform.localPosition.z, 0.0001f);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncSendsSpaceLootWhenSpacePressed()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeSpaceLootSender sender = new FakeSpaceLootSender();
                controller.SetSpaceLootIntentSenderForTests(sender);

                bool dispatched = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, true),
                    0.0f);

                Assert.IsTrue(dispatched);
                Assert.AreEqual(1, sender.SendCallCount);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncSkipsSpaceLootWhenSpaceIsNotPressed()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeSpaceLootSender sender = new FakeSpaceLootSender();
                controller.SetSpaceLootIntentSenderForTests(sender);
                FakeMoveIntentSender moveSender = new FakeMoveIntentSender();
                controller.SetMoveIntentSenderForTests(moveSender);

                bool dispatched = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(1.0f, 0.0f, false),
                    0.5f);

                Assert.IsTrue(dispatched);
                Assert.AreEqual(1, moveSender.SendCallCount);
                Assert.AreEqual(0, sender.SendCallCount);
                Assert.AreEqual(0.5f, avatar.transform.localPosition.x, 0.0001f);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncSuppressesOverlappingSpaceLootSend()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeSpaceLootSender sender = new FakeSpaceLootSender
                {
                    ResultToReturn = new TaskCompletionSource<bool>()
                };
                controller.SetSpaceLootIntentSenderForTests(sender);

                Task<bool> first = controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, true),
                    0.0f);
                bool second = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, true),
                    0.0f);
                sender.ResultToReturn.SetResult(true);

                Assert.IsTrue(await first);
                Assert.IsFalse(second);
                Assert.AreEqual(1, sender.SendCallCount);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncSendsAttackWhenAttackPressed()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeAttackCommandSender sender = new FakeAttackCommandSender();
                controller.SetAttackIntentSenderForTests(sender);

                bool dispatched = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false),
                    attackPressed: true,
                    deltaSeconds: 0.0f);

                Assert.IsTrue(dispatched);
                Assert.AreEqual(1, sender.AttackCallCount);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncSuppressesOverlappingAttackSend()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeAttackCommandSender sender = new FakeAttackCommandSender
                {
                    AttackResultToReturn = new TaskCompletionSource<bool>()
                };
                controller.SetAttackIntentSenderForTests(sender);

                Task<bool> first = controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false),
                    attackPressed: true,
                    deltaSeconds: 0.0f);
                bool second = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false),
                    attackPressed: true,
                    deltaSeconds: 0.0f);
                sender.AttackResultToReturn.SetResult(true);

                Assert.IsTrue(await first);
                Assert.IsFalse(second);
                Assert.AreEqual(1, sender.AttackCallCount);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncReturnsFalseAndClearsInFlightWhenSpaceLootSendThrows()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeSpaceLootSender sender = new FakeSpaceLootSender
                {
                    ThrowOnSend = true
                };
                controller.SetSpaceLootIntentSenderForTests(sender);

                bool first = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, true),
                    0.0f);
                sender.ThrowOnSend = false;
                bool second = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, true),
                    0.0f);

                Assert.IsFalse(first);
                Assert.IsTrue(second);
                Assert.AreEqual(2, sender.SendCallCount);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncSendsMoveWhenMovementAxisChanges()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeMoveIntentSender sender = new FakeMoveIntentSender();
                controller.SetMoveIntentSenderForTests(sender);

                bool first = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(1.0f, 0.0f, false),
                    0.0f);
                bool duplicate = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(1.0f, 0.0f, false),
                    0.0f);
                bool changed = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 1.0f, false),
                    0.0f);

                Assert.IsTrue(first);
                Assert.IsFalse(duplicate);
                Assert.IsTrue(changed);
                Assert.AreEqual(2, sender.SendCallCount);
                Assert.AreEqual(0, sender.Intents[1].MoveX);
                Assert.AreEqual(1, sender.Intents[1].MoveZ);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncSendsStopAfterPreviousMove()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeMoveIntentSender sender = new FakeMoveIntentSender();
                controller.SetMoveIntentSenderForTests(sender);

                await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(-1.0f, 0.0f, false),
                    0.0f);
                bool stopped = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false),
                    0.0f);

                Assert.IsTrue(stopped);
                Assert.AreEqual(2, sender.SendCallCount);
                Assert.AreEqual(0, sender.Intents[1].MoveX);
                Assert.AreEqual(0, sender.Intents[1].MoveZ);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        [Test]
        public async Task StepAndDispatchAsyncSendsIdleHeartbeatWithoutInput()
        {
            GameObject avatar = new GameObject("Avatar");
            try
            {
                PlayerLocalController controller = avatar.AddComponent<PlayerLocalController>();
                FakeMoveIntentSender sender = new FakeMoveIntentSender();
                controller.SetMoveIntentSenderForTests(sender);

                bool first = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false),
                    0.2f);
                bool tooSoon = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false),
                    0.1f);
                bool second = await controller.StepAndDispatchAsync(
                    PlayerInputIntent.FromKeyboard(0.0f, 0.0f, false),
                    0.1f);

                Assert.IsTrue(first);
                Assert.IsFalse(tooSoon);
                Assert.IsTrue(second);
                Assert.AreEqual(2, sender.SendCallCount);
                Assert.AreEqual(0, sender.Intents[0].MoveX);
                Assert.AreEqual(0, sender.Intents[0].MoveZ);
                Assert.AreEqual(0, sender.Intents[1].MoveX);
                Assert.AreEqual(0, sender.Intents[1].MoveZ);
            }
            finally
            {
                Object.DestroyImmediate(avatar);
            }
        }

        private sealed class FakeAttackCommandSender : IPlayerManualCombatCommandSession
        {
            public int AttackCallCount;
            public TaskCompletionSource<bool> AttackResultToReturn;

            public Task<bool> SendAttackIntentAsync()
            {
                ++AttackCallCount;
                return AttackResultToReturn != null ?
                    AttackResultToReturn.Task :
                    Task.FromResult(true);
            }

            public Task<bool> CaptureMonsterHealthSnapshotAsync()
            {
                return Task.FromResult(false);
            }

            public Task<bool> CaptureMonsterDeathAsync()
            {
                return Task.FromResult(false);
            }

            public Task<bool> CaptureDropListSnapshotV2Async()
            {
                return Task.FromResult(false);
            }
        }

        private sealed class FakeSpaceLootSender : IPlayerSpaceLootIntentSender
        {
            public int SendCallCount;
            public TaskCompletionSource<bool> ResultToReturn;
            public bool ThrowOnSend;

            public Task<bool> SendSpaceLootIntentAsync()
            {
                ++SendCallCount;
                if (ThrowOnSend)
                {
                    throw new System.InvalidOperationException("send failed");
                }

                return ResultToReturn != null ?
                    ResultToReturn.Task :
                    Task.FromResult(true);
            }
        }

        private sealed class FakeMoveIntentSender : IPlayerMoveIntentSender
        {
            public int SendCallCount;
            public System.Collections.Generic.List<PlayerInputIntent> Intents =
                new System.Collections.Generic.List<PlayerInputIntent>();

            public Task<bool> SendMoveIntentAsync(PlayerInputIntent intent)
            {
                ++SendCallCount;
                Intents.Add(intent);
                return Task.FromResult(true);
            }
        }
    }
}
