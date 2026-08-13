using UnityEngine.SceneManagement;

namespace LootOfLegends.Presentation.Common
{
    public sealed class UnityLoginNavigation : ILoginNavigation
    {
        public void ReturnToLogin()
        {
            SceneManager.LoadScene("LoginScene");
        }
    }
}
