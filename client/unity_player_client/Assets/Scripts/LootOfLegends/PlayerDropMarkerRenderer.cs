using System.Collections.Generic;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerDropMarkerRenderer : MonoBehaviour
    {
        public const float FixedToUnityDisplayScale =
            1.0f / PlayerRudpStateSnapshotPlayer.PositionScale;
        public const float DefaultMarkerHeight = 0.65f;
        public const float DefaultMarkerScale = 0.75f;
        public const float IdentityDiscWorldDiameter = 0.20f;
        public const float IdentityDiscScale =
            IdentityDiscWorldDiameter / DefaultMarkerScale;
        private const float IdentityDiscWorldCenterHeight = 0.065f;
        private const float IdentityDiscLocalHeight =
            (IdentityDiscWorldCenterHeight - DefaultMarkerHeight) /
            DefaultMarkerScale;

        private static readonly Color DropColor =
            new Color(0.95f, 0.84f, 0.22f, 1.0f);

        [SerializeField]
        private PlayerNetworkSessionController networkSessionController;

        private readonly Dictionary<uint, GameObject> markersByDropId =
            new Dictionary<uint, GameObject>();

        public PlayerNetworkSessionController NetworkSessionController
        {
            get => networkSessionController;
            set => networkSessionController = value;
        }

        public int MarkerCount => markersByDropId.Count;

        public GameObject GetMarker(uint dropId)
        {
            return markersByDropId.TryGetValue(dropId, out GameObject marker) ?
                marker :
                null;
        }

        public void RenderCapturedDrops()
        {
            if (networkSessionController == null ||
                !networkSessionController.DropListSnapshotV2Captured)
            {
                Clear();
                return;
            }

            RenderDrops(networkSessionController.DropListSnapshotV2Drops);
        }

        public void RenderDrops(PlayerDropEntryV2[] drops)
        {
            if (drops == null || drops.Length == 0)
            {
                Clear();
                return;
            }

            HashSet<uint> liveDropIds = new HashSet<uint>();
            foreach (PlayerDropEntryV2 drop in drops)
            {
                if (drop.DropId == 0U)
                {
                    continue;
                }

                liveDropIds.Add(drop.DropId);
                GameObject marker = GetOrCreateMarker(drop.DropId);
                marker.transform.localPosition = FixedToUnityPosition(drop.PosX, drop.PosY);
                marker.transform.localScale = Vector3.one * DefaultMarkerScale;
            }

            RemoveStaleMarkers(liveDropIds);
        }

        public bool ApplyLootResolved(PlayerLootResolved lootResolved)
        {
            if (lootResolved.DropId == 0U ||
                !markersByDropId.TryGetValue(lootResolved.DropId, out GameObject marker))
            {
                return false;
            }

            markersByDropId.Remove(lootResolved.DropId);
            DestroyMarker(marker);
            return true;
        }

        public bool ApplyCapturedLootResolved()
        {
            if (networkSessionController == null ||
                !networkSessionController.LootResolvedCaptured)
            {
                return false;
            }

            return ApplyLootResolved(networkSessionController.LootResolved);
        }

        public void Clear()
        {
            foreach (GameObject marker in markersByDropId.Values)
            {
                DestroyMarker(marker);
            }

            markersByDropId.Clear();
        }

        private GameObject GetOrCreateMarker(uint dropId)
        {
            if (markersByDropId.TryGetValue(dropId, out GameObject marker) &&
                marker != null)
            {
                return marker;
            }

            marker = Release0VisualProviders.Current.CreateDrop();
            marker.name = $"DropMarker_{dropId}";
            marker.transform.SetParent(transform, false);
            Release0VisualMaterialUtility.RemoveRuntimeColliders(marker);
            Release0VisualMaterialUtility.ApplyFallbackMaterialIfRootRenderer(
                marker,
                "DropMarkerMaterial",
                DropColor);
            Release0VisualMaterialUtility.CreateIdentityDisc(
                marker.transform,
                "DropIdentityMarker",
                DropColor,
                new Vector3(0.0f, IdentityDiscLocalHeight, 0.0f),
                IdentityDiscScale);
            markersByDropId[dropId] = marker;
            return marker;
        }

        private void RemoveStaleMarkers(HashSet<uint> liveDropIds)
        {
            List<uint> staleDropIds = new List<uint>();
            foreach (uint dropId in markersByDropId.Keys)
            {
                if (!liveDropIds.Contains(dropId))
                {
                    staleDropIds.Add(dropId);
                }
            }

            foreach (uint dropId in staleDropIds)
            {
                GameObject marker = markersByDropId[dropId];
                markersByDropId.Remove(dropId);
                DestroyMarker(marker);
            }
        }

        private static Vector3 FixedToUnityPosition(int posX, int posY)
        {
            return new Vector3(
                posX * FixedToUnityDisplayScale,
                DefaultMarkerHeight,
                posY * FixedToUnityDisplayScale);
        }

        private void Update()
        {
            RenderCapturedDrops();
            ApplyCapturedLootResolved();
        }

        private static void DestroyMarker(GameObject marker)
        {
            if (marker == null)
            {
                return;
            }

            if (Application.isPlaying)
            {
                Destroy(marker);
            }
            else
            {
                DestroyImmediate(marker);
            }
        }

        private void OnDestroy()
        {
            Clear();
        }
    }
}
