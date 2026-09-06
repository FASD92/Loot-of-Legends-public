using System;
using UnityEngine;

namespace LootOfLegends.Presentation.Arena
{
    [RequireComponent(typeof(LineRenderer))]
    public sealed class ArenaAttackProjectile : MonoBehaviour
    {
        private const float TravelSeconds = 0.18f;
        private Vector3 start;
        private Vector3 end;
        private float elapsed;
        private LineRenderer line;
        private Material material;
        private Action onImpact;
        private bool impacted;

        public void Begin(Vector3 from, Vector3 to, Action hit)
        {
            start = from;
            end = to;
            onImpact = hit;
            line = GetComponent<LineRenderer>();
            material = new Material(Shader.Find("Sprites/Default"));
            line.material = material;
            line.positionCount = 2;
            line.startWidth = 0.16f;
            line.endWidth = 0.06f;
            line.startColor = new Color(1f, 0.75f, 0.2f, 1f);
            line.endColor = Color.white;
            line.sortingOrder = 20;
            Project(0f);
        }

        private void Update()
        {
            if (line == null || impacted)
            {
                return;
            }
            elapsed += Time.deltaTime;
            float progress = Mathf.Clamp01(elapsed / TravelSeconds);
            Project(progress);
            if (progress < 1f)
            {
                return;
            }
            impacted = true;
            onImpact?.Invoke();
            Destroy(gameObject, 0.12f);
        }

        private void OnDestroy()
        {
            if (material != null)
            {
                Destroy(material);
            }
        }

        private void Project(float progress)
        {
            Vector3 head = Vector3.Lerp(start, end, progress);
            Vector3 tail = Vector3.Lerp(start, end, Mathf.Max(0f, progress - 0.2f));
            line.SetPosition(0, tail);
            line.SetPosition(1, head);
        }
    }
}
