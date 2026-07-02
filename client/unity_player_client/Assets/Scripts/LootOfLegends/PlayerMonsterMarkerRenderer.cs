using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerMonsterMarkerRenderer : MonoBehaviour
    {
        public static readonly Vector3 DefaultMonsterPosition =
            new Vector3(0.0f, 1.0f, 3.0f);
        public const float DefaultMonsterScale = 1.3f;
        public static readonly Vector3 HealthBarLocalPosition =
            new Vector3(0.0f, 2.15f, -0.15f);
        public static readonly Quaternion HealthBarLocalRotation =
            Quaternion.Euler(65.0f, 0.0f, 0.0f);
        public static readonly Vector3 HealthBarLocalScale =
            new Vector3(1.35f, 1.0f, 1.0f);

        private static readonly Color MonsterColor =
            new Color(0.88f, 0.24f, 0.20f, 1.0f);

        [SerializeField]
        private PlayerNetworkSessionController networkSessionController;

        private GameObject marker;
        private uint markerMonsterId;
        private PlayerMonsterHealthBar healthBar;

        public PlayerNetworkSessionController NetworkSessionController
        {
            get => networkSessionController;
            set => networkSessionController = value;
        }

        public bool HasMarker => marker != null;

        public GameObject Marker => marker;

        public PlayerMonsterHealthBar HealthBar => healthBar;

        public void RenderCapturedMonster()
        {
            if (networkSessionController == null ||
                !networkSessionController.MonsterSpawned ||
                (networkSessionController.MonsterDeathCaptured &&
                 networkSessionController.MonsterDeathMonsterId ==
                 networkSessionController.MonsterId))
            {
                Clear();
                return;
            }

            ushort currentHp = networkSessionController.MonsterMaxHp;
            ushort maxHp = networkSessionController.MonsterMaxHp;
            if (networkSessionController.MonsterHealthSnapshotCaptured &&
                networkSessionController.MonsterHealthMonsterId ==
                networkSessionController.MonsterId)
            {
                currentHp = networkSessionController.MonsterCurrentHp;
                maxHp = networkSessionController.MonsterHealthMaxHp;
            }

            RenderMonster(
                networkSessionController.MonsterSpawnRoomId,
                networkSessionController.MonsterId,
                networkSessionController.MonsterTypeId,
                currentHp,
                maxHp);
        }

        public void RenderMonster(
            uint roomId,
            uint monsterId,
            uint monsterTypeId,
            ushort maxHp)
        {
            RenderMonster(roomId, monsterId, monsterTypeId, maxHp, maxHp);
        }

        public void RenderMonster(
            uint roomId,
            uint monsterId,
            uint monsterTypeId,
            ushort currentHp,
            ushort maxHp)
        {
            if (roomId == 0U ||
                monsterId == 0U ||
                monsterTypeId == 0U ||
                maxHp == 0 ||
                currentHp > maxHp)
            {
                Clear();
                return;
            }

            GameObject nextMarker = GetOrCreateMarker(monsterId);
            nextMarker.transform.localPosition = DefaultMonsterPosition;
            nextMarker.transform.localScale = Vector3.one * DefaultMonsterScale;
            RenderHealthBar(nextMarker, currentHp, maxHp);
        }

        public void Clear()
        {
            DestroyMarker(marker);
            marker = null;
            markerMonsterId = 0U;
            healthBar = null;
        }

        private void Update()
        {
            RenderCapturedMonster();
        }

        private GameObject GetOrCreateMarker(uint monsterId)
        {
            if (marker != null && markerMonsterId == monsterId)
            {
                return marker;
            }

            Clear();
            marker = Release0VisualProviders.Current.CreateMonster();
            marker.name = $"Monster_{monsterId}";
            marker.transform.SetParent(transform, false);
            markerMonsterId = monsterId;
            Release0VisualMaterialUtility.RemoveRuntimeColliders(marker);
            Release0VisualMaterialUtility.ApplyFallbackMaterialIfRootRenderer(
                marker,
                "MonsterMaterial",
                MonsterColor);
            Release0VisualMaterialUtility.CreateIdentityDisc(
                marker.transform,
                "MonsterIdentityMarker",
                MonsterColor,
                new Vector3(0.0f, -0.75f, 0.0f),
                0.95f);
            return marker;
        }

        private void RenderHealthBar(GameObject parent, ushort currentHp, ushort maxHp)
        {
            if (healthBar == null)
            {
                GameObject barObject = new GameObject("MonsterHealthBar");
                barObject.transform.SetParent(parent.transform, false);
                healthBar = barObject.AddComponent<PlayerMonsterHealthBar>();
            }

            healthBar.Render(currentHp, maxHp);
            healthBar.transform.localPosition = HealthBarLocalPosition;
            healthBar.transform.localRotation = HealthBarLocalRotation;
            healthBar.transform.localScale = HealthBarLocalScale;
        }

        private static void DestroyMarker(GameObject markerObject)
        {
            if (markerObject == null)
            {
                return;
            }

            if (Application.isPlaying)
            {
                Destroy(markerObject);
            }
            else
            {
                DestroyImmediate(markerObject);
            }
        }

        private void OnDestroy()
        {
            Clear();
        }
    }
}
