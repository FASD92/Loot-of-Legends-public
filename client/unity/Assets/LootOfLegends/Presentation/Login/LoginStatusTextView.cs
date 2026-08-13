using UnityEngine;

namespace LootOfLegends.Presentation.Login
{
    [RequireComponent(typeof(TextMesh))]
    public sealed class LoginStatusTextView : MonoBehaviour
    {
        private TextMesh label;

        private void Awake()
        {
            label = GetComponent<TextMesh>();
            if (label == null)
            {
                label = gameObject.AddComponent<TextMesh>();
            }
            ShowStatus("로그인 준비 중입니다.");
        }

        public void ShowStatus(string copy)
        {
            label.text = copy ?? string.Empty;
        }
    }
}
