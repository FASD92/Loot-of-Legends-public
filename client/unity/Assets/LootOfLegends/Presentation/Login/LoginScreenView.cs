using UnityEngine;

namespace LootOfLegends.Presentation.Login
{
    public sealed class LoginScreenView : MonoBehaviour
    {
        [SerializeField] private GameObject panel;
        [SerializeField] private LoginStatusTextView statusView;

        private void Awake()
        {
            if (panel != null)
            {
                panel.SetActive(true);
            }
        }
    }
}
