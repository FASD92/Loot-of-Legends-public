using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Common
{
    public sealed class SafeFailureTextView : MonoBehaviour,
        ISafeFailureView,
        IBattleRecoveryView
    {
        [SerializeField] private GameObject panel;
        [SerializeField] private Text label;

        private static SafeFailureTextView instance;

        private void Awake()
        {
            if (instance != null && instance != this)
            {
                Destroy(gameObject);
                return;
            }

            instance = this;
            DontDestroyOnLoad(gameObject);
            HideBlockingMessage();
        }

        private void OnDestroy()
        {
            if (instance == this)
            {
                instance = null;
            }
        }

        public void ShowBlockingMessage(string copy)
        {
            label.text = copy ?? string.Empty;
            panel.SetActive(true);
        }

        public void HideBlockingMessage()
        {
            label.text = string.Empty;
            panel.SetActive(false);
        }
    }
}
