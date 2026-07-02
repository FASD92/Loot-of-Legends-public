using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerCameraFollow : MonoBehaviour
    {
        [SerializeField]
        private Transform target;

        [SerializeField]
        private Vector3 offset = new Vector3(0.0f, 12.0f, -10.0f);

        public Transform Target
        {
            get => target;
            set => target = value;
        }

        public Vector3 Offset
        {
            get => offset;
            set => offset = value;
        }

        private void LateUpdate()
        {
            ApplyFollow();
        }

        public void ApplyFollow()
        {
            if (target == null)
            {
                return;
            }

            transform.position = target.position + offset;
            transform.LookAt(target.position);
        }
    }
}
