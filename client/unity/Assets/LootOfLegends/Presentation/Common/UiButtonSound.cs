using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Common
{
    [RequireComponent(typeof(Button), typeof(AudioSource))]
    public sealed class UiButtonSound : MonoBehaviour
    {
        private const string ClickSoundKey = "audio.sfx.ui-click";

        [SerializeField] private PresentationCatalog catalog;
        [SerializeField] private AudioSource audioSource;

        private Button button;
        private AudioClip clip;

        private void Awake()
        {
            button = GetComponent<Button>();
            if (catalog != null && audioSource != null)
            {
                clip = catalog.Resolve<AudioClip>(ClickSoundKey);
            }
            button.onClick.AddListener(Play);
        }

        private void OnDestroy()
        {
            if (button != null)
            {
                button.onClick.RemoveListener(Play);
            }
        }

        private void Play()
        {
            if (clip != null)
            {
                audioSource.PlayOneShot(clip);
            }
        }
    }
}
