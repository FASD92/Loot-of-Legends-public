using System.Collections.Generic;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerBattleParticipantRenderer : MonoBehaviour
    {
        public static readonly Vector3 DefaultRemotePosition =
            new Vector3(2.5f, 1.1f, 0.0f);
        public const float DefaultMarkerScale = 0.75f;

        private static readonly Color RemoteParticipantColor =
            new Color(0.24f, 0.56f, 0.95f, 1.0f);

        [SerializeField]
        private PlayerNetworkSessionController networkSessionController;

        private readonly Dictionary<ulong, GameObject> remoteMarkersBySessionId =
            new Dictionary<ulong, GameObject>();

        public PlayerNetworkSessionController NetworkSessionController
        {
            get => networkSessionController;
            set => networkSessionController = value;
        }

        public int RemoteMarkerCount => remoteMarkersBySessionId.Count;

        public GameObject GetRemoteMarker(ulong sessionId)
        {
            return remoteMarkersBySessionId.TryGetValue(sessionId, out GameObject marker) ?
                marker :
                null;
        }

        public void RenderCapturedParticipants()
        {
            if (networkSessionController == null ||
                !networkSessionController.BattleStarted)
            {
                Clear();
                return;
            }

            if (networkSessionController.StateSnapshotCaptured)
            {
                RenderStateSnapshotParticipants(
                    networkSessionController.SessionId,
                    networkSessionController.StateSnapshot);
                return;
            }

            RenderParticipants(
                networkSessionController.SessionId,
                networkSessionController.BattleStartPlayerASessionId,
                networkSessionController.BattleStartPlayerBSessionId);
        }

        public void RenderParticipants(
            ulong selfSessionId,
            ulong playerASessionId,
            ulong playerBSessionId)
        {
            ulong remoteSessionId = ResolveRemoteSessionId(
                selfSessionId,
                playerASessionId,
                playerBSessionId);
            if (remoteSessionId == 0UL)
            {
                Clear();
                return;
            }

            GameObject marker = GetOrCreateRemoteMarker(remoteSessionId);
            marker.transform.localPosition = DefaultRemotePosition;
            marker.transform.localScale = Vector3.one * DefaultMarkerScale;
            RemoveStaleMarkers(remoteSessionId);
        }

        public void RenderStateSnapshotParticipants(
            ulong selfSessionId,
            PlayerRudpStateSnapshot snapshot)
        {
            if (selfSessionId == 0UL || !snapshot.IsValid)
            {
                Clear();
                return;
            }

            HashSet<ulong> liveSessionIds = new HashSet<ulong>();
            PlayerRudpStateSnapshotPlayer[] players = snapshot.Players;
            for (int index = 0; index < players.Length; ++index)
            {
                PlayerRudpStateSnapshotPlayer player = players[index];
                if (!player.IsValid || player.SessionId == selfSessionId)
                {
                    continue;
                }

                GameObject marker = GetOrCreateRemoteMarker(player.SessionId);
                marker.transform.localPosition =
                    new Vector3(player.WorldX, DefaultRemotePosition.y, player.WorldZ);
                marker.transform.localScale = Vector3.one * DefaultMarkerScale;
                liveSessionIds.Add(player.SessionId);
            }

            RemoveStaleMarkers(liveSessionIds);
        }

        private void Update()
        {
            RenderCapturedParticipants();
        }

        private GameObject GetOrCreateRemoteMarker(ulong sessionId)
        {
            if (remoteMarkersBySessionId.TryGetValue(sessionId, out GameObject marker) &&
                marker != null)
            {
                return marker;
            }

            marker = Release0VisualProviders.Current.CreateRemotePlayer();
            marker.name = $"RemoteParticipant_{sessionId}";
            marker.transform.SetParent(transform, false);
            Release0VisualMaterialUtility.RemoveRuntimeColliders(marker);
            Release0VisualMaterialUtility.ApplyFallbackMaterialIfRootRenderer(
                marker,
                "RemoteParticipantMaterial",
                RemoteParticipantColor);
            Release0VisualMaterialUtility.CreateIdentityDisc(
                marker.transform,
                "RemoteParticipantIdentityMarker",
                RemoteParticipantColor,
                new Vector3(0.0f, -1.4f, 0.0f),
                0.9f);
            remoteMarkersBySessionId[sessionId] = marker;
            return marker;
        }

        private void RemoveStaleMarkers(ulong liveSessionId)
        {
            List<ulong> staleSessionIds = new List<ulong>();
            foreach (ulong sessionId in remoteMarkersBySessionId.Keys)
            {
                if (sessionId != liveSessionId)
                {
                    staleSessionIds.Add(sessionId);
                }
            }

            foreach (ulong sessionId in staleSessionIds)
            {
                GameObject marker = remoteMarkersBySessionId[sessionId];
                remoteMarkersBySessionId.Remove(sessionId);
                DestroyMarker(marker);
            }
        }

        private void RemoveStaleMarkers(HashSet<ulong> liveSessionIds)
        {
            List<ulong> staleSessionIds = new List<ulong>();
            foreach (ulong sessionId in remoteMarkersBySessionId.Keys)
            {
                if (!liveSessionIds.Contains(sessionId))
                {
                    staleSessionIds.Add(sessionId);
                }
            }

            foreach (ulong sessionId in staleSessionIds)
            {
                GameObject marker = remoteMarkersBySessionId[sessionId];
                remoteMarkersBySessionId.Remove(sessionId);
                DestroyMarker(marker);
            }
        }

        public void Clear()
        {
            foreach (GameObject marker in remoteMarkersBySessionId.Values)
            {
                DestroyMarker(marker);
            }

            remoteMarkersBySessionId.Clear();
        }

        private static ulong ResolveRemoteSessionId(
            ulong selfSessionId,
            ulong playerASessionId,
            ulong playerBSessionId)
        {
            if (selfSessionId == 0UL ||
                playerASessionId == 0UL ||
                playerBSessionId == 0UL ||
                playerASessionId == playerBSessionId)
            {
                return 0UL;
            }

            if (selfSessionId == playerASessionId)
            {
                return playerBSessionId;
            }

            return selfSessionId == playerBSessionId ? playerASessionId : 0UL;
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
