using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class Release0PlayerVisualAnimator : MonoBehaviour
    {
        public const string IsMovingParameter = "IsMoving";

        [SerializeField]
        private Animator animator;

        [SerializeField]
        private float movementEpsilon = 0.001f;

        private Vector3 lastPosition;
        private bool hasLastPosition;
        private int explicitMovementFrame = -1;

        public Animator Animator
        {
            get => animator;
            set => animator = value;
        }

        public void SetMoving(bool isMoving)
        {
            Animator resolvedAnimator = ResolveAnimator();
            if (resolvedAnimator != null)
            {
                resolvedAnimator.SetBool(IsMovingParameter, isMoving);
            }
        }

        public void ApplyMovementIntent(PlayerInputIntent intent)
        {
            ApplyMovementVector(new Vector3(intent.MoveX, 0.0f, intent.MoveZ));
        }

        public void ApplyMovementVector(Vector3 movement)
        {
            explicitMovementFrame = Time.frameCount;
            ApplyVisualMovement(movement);
        }

        private void Awake()
        {
            ResolveAnimator();
            CapturePosition();
        }

        private void LateUpdate()
        {
            Vector3 currentPosition = transform.position;
            if (hasLastPosition && explicitMovementFrame != Time.frameCount)
            {
                ApplyVisualMovement(currentPosition - lastPosition);
            }

            CapturePosition();
        }

        private void ApplyVisualMovement(Vector3 movement)
        {
            Vector3 flatMovement = new Vector3(movement.x, 0.0f, movement.z);
            bool isMoving =
                flatMovement.sqrMagnitude > movementEpsilon * movementEpsilon;
            SetMoving(isMoving);
            if (!isMoving)
            {
                return;
            }

            Animator resolvedAnimator = ResolveAnimator();
            if (resolvedAnimator != null)
            {
                resolvedAnimator.transform.rotation =
                    Quaternion.LookRotation(flatMovement.normalized, Vector3.up);
            }
        }

        private void CapturePosition()
        {
            lastPosition = transform.position;
            hasLastPosition = true;
        }

        private Animator ResolveAnimator()
        {
            if (animator == null)
            {
                animator = GetComponentInChildren<Animator>(true);
            }

            return animator;
        }
    }
}
