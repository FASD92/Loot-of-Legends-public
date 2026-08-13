using UnityEngine;

namespace LootOfLegends.Presentation.Common
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class SafeFailureTextView : MonoBehaviour,
        ISafeFailureView,
        IBattleRecoveryView
    {
        private TextMesh label;

        private void Awake()
        {
            label = GetComponent<TextMesh>();
            if (label == null)
            {
                label = gameObject.AddComponent<TextMesh>();
            }
        }

        public void ShowBlockingMessage(string copy)
        {
            gameObject.SetActive(true);
            label.text = copy ?? string.Empty;
        }

        public void HideBlockingMessage()
        {
            label.text = string.Empty;
            gameObject.SetActive(false);
        }
    }
}
