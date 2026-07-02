using System.Threading.Tasks;
using UnityEngine;
using UnityEngine.InputSystem;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerLocalController : MonoBehaviour
    {
        private const float IdleMoveHeartbeatIntervalSeconds = 0.2f;

        [SerializeField]
        private float speedUnitsPerSecond = PlayerLocalMovement.DefaultSpeedUnitsPerSecond;

        [SerializeField]
        private float arenaClampHalfExtent = PlayerLocalMovement.DefaultArenaClampHalfExtent;

        [SerializeField]
        private PlayerNetworkSessionController networkSessionController;

        private IPlayerSpaceLootIntentSender testSpaceLootIntentSender;
        private IPlayerMoveIntentSender testMoveIntentSender;
        private IPlayerManualCombatCommandSession testAttackIntentSender;
        private bool spaceLootSendInFlight;
        private bool moveSendInFlight;
        private bool attackSendInFlight;
        private short lastSentMoveX;
        private short lastSentMoveZ;
        private bool hasSentMoveIntent;
        private float idleMoveHeartbeatElapsedSeconds;
        private Release0PlayerVisualAnimator visualAnimator;

        public float SpeedUnitsPerSecond
        {
            get => speedUnitsPerSecond;
            set => speedUnitsPerSecond = value;
        }

        public float ArenaClampHalfExtent
        {
            get => arenaClampHalfExtent;
            set => arenaClampHalfExtent = value;
        }

        public PlayerNetworkSessionController NetworkSessionController
        {
            get => networkSessionController;
            set => networkSessionController = value;
        }

        private void Update()
        {
            Keyboard keyboard = Keyboard.current;
            Mouse mouse = Mouse.current;
            bool attackPressed = mouse != null &&
                mouse.leftButton.wasPressedThisFrame;
            if (keyboard == null && !attackPressed)
            {
                return;
            }

            PlayerInputIntent intent = BuildIntentFromDigitalInput(
                keyboard != null &&
                    (keyboard.aKey.isPressed || keyboard.leftArrowKey.isPressed),
                keyboard != null &&
                    (keyboard.dKey.isPressed || keyboard.rightArrowKey.isPressed),
                keyboard != null &&
                    (keyboard.sKey.isPressed || keyboard.downArrowKey.isPressed),
                keyboard != null &&
                    (keyboard.wKey.isPressed || keyboard.upArrowKey.isPressed),
                keyboard != null &&
                    keyboard.spaceKey.wasPressedThisFrame);

            _ = StepAndDispatchAsync(intent, attackPressed, Time.deltaTime);
        }

        public static PlayerInputIntent BuildIntentFromDigitalInput(
            bool leftPressed,
            bool rightPressed,
            bool backwardPressed,
            bool forwardPressed,
            bool spaceLootPressed)
        {
            float horizontal = 0.0f;
            if (leftPressed)
            {
                horizontal -= 1.0f;
            }

            if (rightPressed)
            {
                horizontal += 1.0f;
            }

            float vertical = 0.0f;
            if (backwardPressed)
            {
                vertical -= 1.0f;
            }

            if (forwardPressed)
            {
                vertical += 1.0f;
            }

            return PlayerInputIntent.FromKeyboard(horizontal, vertical, spaceLootPressed);
        }

        public void Step(PlayerInputIntent intent, float deltaSeconds)
        {
            transform.localPosition = PlayerLocalMovement.Apply(
                transform.localPosition,
                intent,
                speedUnitsPerSecond,
                deltaSeconds,
                arenaClampHalfExtent);
            ApplyVisualMovementIntent(intent);
        }

        public void SetSpaceLootIntentSenderForTests(IPlayerSpaceLootIntentSender sender)
        {
            testSpaceLootIntentSender = sender;
        }

        public void SetMoveIntentSenderForTests(IPlayerMoveIntentSender sender)
        {
            testMoveIntentSender = sender;
        }

        public void SetAttackIntentSenderForTests(IPlayerManualCombatCommandSession sender)
        {
            testAttackIntentSender = sender;
        }

        public async Task<bool> StepAndDispatchAsync(PlayerInputIntent intent, float deltaSeconds)
        {
            return await StepAndDispatchAsync(intent, false, deltaSeconds);
        }

        public async Task<bool> StepAndDispatchAsync(
            PlayerInputIntent intent,
            bool attackPressed,
            float deltaSeconds)
        {
            Step(intent, deltaSeconds);
            ApplyCapturedAuthoritativeSelfPosition();

            bool sent = await DispatchMoveIntentIfChangedAsync(intent, deltaSeconds);
            sent = await DispatchAttackIntentIfPressedAsync(attackPressed) || sent;
            if (!intent.SpaceLootPressed || spaceLootSendInFlight)
            {
                return sent;
            }

            IPlayerSpaceLootIntentSender sender = ResolveSpaceLootSender();
            if (sender == null)
            {
                return sent;
            }

            spaceLootSendInFlight = true;
            try
            {
                return await sender.SendSpaceLootIntentAsync() || sent;
            }
            catch (System.InvalidOperationException)
            {
                return sent;
            }
            finally
            {
                spaceLootSendInFlight = false;
            }
        }

        private async Task<bool> DispatchAttackIntentIfPressedAsync(bool attackPressed)
        {
            if (!attackPressed || attackSendInFlight)
            {
                return false;
            }

            IPlayerManualCombatCommandSession sender = ResolveAttackSender();
            if (sender == null)
            {
                return false;
            }

            attackSendInFlight = true;
            try
            {
                return await sender.SendAttackIntentAsync();
            }
            catch (System.InvalidOperationException)
            {
                return false;
            }
            finally
            {
                attackSendInFlight = false;
            }
        }

        public bool ApplyAuthoritativeSelfPosition(
            PlayerRudpStateSnapshot snapshot,
            ulong selfSessionId)
        {
            if (selfSessionId == 0UL ||
                !snapshot.TryGetPlayer(
                    selfSessionId,
                    out PlayerRudpStateSnapshotPlayer self) ||
                !self.IsValid)
            {
                return false;
            }

            Vector3 next = transform.localPosition;
            next.x = self.WorldX;
            next.z = self.WorldZ;
            transform.localPosition = next;
            return true;
        }

        private bool ApplyCapturedAuthoritativeSelfPosition()
        {
            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            if (controller == null || !controller.StateSnapshotCaptured)
            {
                return false;
            }

            return ApplyAuthoritativeSelfPosition(
                controller.StateSnapshot,
                controller.SessionId);
        }

        private async Task<bool> DispatchMoveIntentIfChangedAsync(
            PlayerInputIntent intent,
            float deltaSeconds)
        {
            if (moveSendInFlight || !ShouldSendMoveIntent(intent, deltaSeconds))
            {
                return false;
            }

            IPlayerMoveIntentSender sender = ResolveMoveSender();
            if (sender == null)
            {
                return false;
            }

            moveSendInFlight = true;
            try
            {
                bool sent = await sender.SendMoveIntentAsync(intent);
                if (sent)
                {
                    lastSentMoveX = intent.MoveX;
                    lastSentMoveZ = intent.MoveZ;
                    hasSentMoveIntent = true;
                    idleMoveHeartbeatElapsedSeconds = 0.0f;
                }

                return sent;
            }
            catch (System.InvalidOperationException)
            {
                return false;
            }
            finally
            {
                moveSendInFlight = false;
            }
        }

        private bool ShouldSendMoveIntent(PlayerInputIntent intent, float deltaSeconds)
        {
            bool hasMovement = intent.MoveX != 0 || intent.MoveZ != 0;
            if (hasMovement)
            {
                idleMoveHeartbeatElapsedSeconds = 0.0f;
                if (!hasSentMoveIntent)
                {
                    return true;
                }

                return intent.MoveX != lastSentMoveX || intent.MoveZ != lastSentMoveZ;
            }

            idleMoveHeartbeatElapsedSeconds += Mathf.Max(0.0f, deltaSeconds);
            if (!hasSentMoveIntent)
            {
                return idleMoveHeartbeatElapsedSeconds >= IdleMoveHeartbeatIntervalSeconds &&
                    CanSendIdleMoveHeartbeat();
            }

            if (intent.MoveX != lastSentMoveX || intent.MoveZ != lastSentMoveZ)
            {
                return true;
            }

            return idleMoveHeartbeatElapsedSeconds >= IdleMoveHeartbeatIntervalSeconds &&
                CanSendIdleMoveHeartbeat();
        }

        private bool CanSendIdleMoveHeartbeat()
        {
            if (testMoveIntentSender != null)
            {
                return true;
            }

            PlayerNetworkSessionController controller = ResolveNetworkSessionController();
            return controller != null &&
                controller.Status == PlayerNetworkSessionStatus.Connected &&
                controller.CurrentRoomId != 0U &&
                controller.BattleStarted;
        }

        private IPlayerMoveIntentSender ResolveMoveSender()
        {
            if (testMoveIntentSender != null)
            {
                return testMoveIntentSender;
            }

            if (networkSessionController != null)
            {
                return networkSessionController;
            }

            networkSessionController = GetComponentInParent<PlayerNetworkSessionController>();
            return networkSessionController;
        }

        private PlayerNetworkSessionController ResolveNetworkSessionController()
        {
            if (networkSessionController != null)
            {
                return networkSessionController;
            }

            networkSessionController = GetComponentInParent<PlayerNetworkSessionController>();
            return networkSessionController;
        }

        private void ApplyVisualMovementIntent(PlayerInputIntent intent)
        {
            Release0PlayerVisualAnimator resolvedAnimator = ResolveVisualAnimator();
            if (resolvedAnimator != null)
            {
                resolvedAnimator.ApplyMovementIntent(intent);
            }
        }

        private Release0PlayerVisualAnimator ResolveVisualAnimator()
        {
            if (visualAnimator != null)
            {
                return visualAnimator;
            }

            visualAnimator = GetComponent<Release0PlayerVisualAnimator>();
            if (visualAnimator == null)
            {
                visualAnimator = GetComponentInChildren<Release0PlayerVisualAnimator>(true);
            }

            return visualAnimator;
        }

        private IPlayerSpaceLootIntentSender ResolveSpaceLootSender()
        {
            if (testSpaceLootIntentSender != null)
            {
                return testSpaceLootIntentSender;
            }

            if (networkSessionController != null)
            {
                return networkSessionController;
            }

            networkSessionController = GetComponentInParent<PlayerNetworkSessionController>();
            return networkSessionController;
        }

        private IPlayerManualCombatCommandSession ResolveAttackSender()
        {
            if (testAttackIntentSender != null)
            {
                return testAttackIntentSender;
            }

            if (networkSessionController != null)
            {
                return networkSessionController;
            }

            networkSessionController = GetComponentInParent<PlayerNetworkSessionController>();
            return networkSessionController;
        }
    }
}
