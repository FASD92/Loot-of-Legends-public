using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Login
{
    public sealed class LoginStatusTextView : MonoBehaviour
    {
        [SerializeField] private Text label;
        private string copy = "로그인 준비 중입니다.";

        private void Awake()
        {
            Render();
        }

        public void ShowStatus(string next)
        {
            copy = next ?? string.Empty;
            Render();
        }

        private void Render()
        {
            if (label != null)
            {
                label.text = copy;
            }
        }
    }
}
