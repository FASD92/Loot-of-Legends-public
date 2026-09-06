using UnityEngine;

namespace LootOfLegends.Presentation.Arena
{
    [RequireComponent(typeof(SpriteRenderer))]
    public sealed class ArenaDropVisual : MonoBehaviour
    {
        [SerializeField] private Sprite commonSprite;
        [SerializeField] private Sprite rareSprite;

        public void Project(ulong itemId)
        {
            GetComponent<SpriteRenderer>().sprite =
                itemId == 2 ? rareSprite : commonSprite;
        }
    }
}
