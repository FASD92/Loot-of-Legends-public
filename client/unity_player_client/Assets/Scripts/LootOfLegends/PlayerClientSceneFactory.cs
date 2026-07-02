using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerClientSceneObjects
    {
        public readonly GameObject ArenaFloor;
        public readonly GameObject PlayerAvatar;
        public readonly Camera MainCamera;
        public readonly Light DirectionalLight;
        public readonly PlayerLocalController PlayerController;
        public readonly PlayerCameraFollow CameraFollow;
        public readonly PlayerNetworkSessionController NetworkSessionController;
        public readonly PlayerBattleParticipantRenderer BattleParticipantRenderer;
        public readonly PlayerMonsterMarkerRenderer MonsterMarkerRenderer;
        public readonly PlayerDropMarkerRenderer DropMarkerRenderer;
        public readonly PlayerInventoryStatusRenderer InventoryStatusRenderer;
        public readonly PlayerLootRaceEvidenceRenderer LootRaceEvidenceRenderer;
        public readonly PrimitiveType ArenaFloorPrimitiveType;
        public readonly PrimitiveType PlayerAvatarPrimitiveType;

        public PlayerClientSceneObjects(
            GameObject arenaFloor,
            GameObject playerAvatar,
            Camera mainCamera,
            Light directionalLight,
            PlayerLocalController playerController,
            PlayerCameraFollow cameraFollow,
            PlayerNetworkSessionController networkSessionController,
            PlayerBattleParticipantRenderer battleParticipantRenderer,
            PlayerMonsterMarkerRenderer monsterMarkerRenderer,
            PlayerDropMarkerRenderer dropMarkerRenderer,
            PlayerInventoryStatusRenderer inventoryStatusRenderer,
            PlayerLootRaceEvidenceRenderer lootRaceEvidenceRenderer,
            PrimitiveType arenaFloorPrimitiveType,
            PrimitiveType playerAvatarPrimitiveType)
        {
            ArenaFloor = arenaFloor;
            PlayerAvatar = playerAvatar;
            MainCamera = mainCamera;
            DirectionalLight = directionalLight;
            PlayerController = playerController;
            CameraFollow = cameraFollow;
            NetworkSessionController = networkSessionController;
            BattleParticipantRenderer = battleParticipantRenderer;
            MonsterMarkerRenderer = monsterMarkerRenderer;
            DropMarkerRenderer = dropMarkerRenderer;
            InventoryStatusRenderer = inventoryStatusRenderer;
            LootRaceEvidenceRenderer = lootRaceEvidenceRenderer;
            ArenaFloorPrimitiveType = arenaFloorPrimitiveType;
            PlayerAvatarPrimitiveType = playerAvatarPrimitiveType;
        }
    }

    public static class PlayerClientSceneFactory
    {
        private const string ArenaFloorMaterialResourcePath = "Materials/ArenaFloorMaterial";
        private const string PlayerAvatarMaterialResourcePath = "Materials/PlayerAvatarMaterial";
        private static readonly Color ArenaFloorColor = new Color(0.18f, 0.42f, 0.32f, 1.0f);
        private static readonly Color PlayerAvatarColor = new Color(0.94f, 0.72f, 0.28f, 1.0f);

        public static PlayerClientSceneObjects Build(Transform root)
        {
            PlayerNetworkSessionController networkSessionController =
                root.GetComponent<PlayerNetworkSessionController>();
            if (networkSessionController == null)
            {
                networkSessionController = root.gameObject.AddComponent<PlayerNetworkSessionController>();
            }

            IRelease0VisualProvider visualProvider = Release0VisualProviders.Current;

            GameObject arenaFloor = visualProvider.CreateArenaFloor();
            arenaFloor.name = "ArenaFloor";
            arenaFloor.transform.SetParent(root, false);
            arenaFloor.transform.localPosition = Vector3.zero;
            float arenaSize = PlayerLocalMovement.DefaultArenaClampHalfExtent * 2.0f;
            if (arenaFloor.GetComponent<Release0ArenaFloorVisual>() == null)
            {
                arenaFloor.transform.localScale = new Vector3(arenaSize, 0.2f, arenaSize);
            }
            else
            {
                arenaFloor.transform.localScale = Vector3.one;
            }
            Release0VisualMaterialUtility.RemoveRuntimeColliders(arenaFloor);
            Release0VisualMaterialUtility.ApplyFallbackMaterialIfRootRenderer(
                arenaFloor,
                "ArenaFloorMaterial",
                ArenaFloorColor,
                ArenaFloorMaterialResourcePath);

            GameObject playerAvatar = visualProvider.CreateLocalPlayer();
            playerAvatar.name = "PlayerAvatar";
            playerAvatar.transform.SetParent(root, false);
            playerAvatar.transform.localPosition = new Vector3(0.0f, 1.1f, 0.0f);
            Release0VisualMaterialUtility.RemoveRuntimeColliders(playerAvatar);
            Release0VisualMaterialUtility.ApplyFallbackMaterialIfRootRenderer(
                playerAvatar,
                "PlayerAvatarMaterial",
                PlayerAvatarColor,
                PlayerAvatarMaterialResourcePath);
            Release0VisualMaterialUtility.CreateIdentityDisc(
                playerAvatar.transform,
                "LocalPlayerIdentityMarker",
                PlayerAvatarColor,
                new Vector3(0.0f, -1.05f, 0.0f),
                0.9f);
            PlayerLocalController playerController =
                playerAvatar.GetComponent<PlayerLocalController>() ??
                playerAvatar.AddComponent<PlayerLocalController>();
            playerController.NetworkSessionController = networkSessionController;

            GameObject cameraObject = new GameObject("Main Camera");
            cameraObject.transform.SetParent(root, false);
            cameraObject.transform.localPosition = new Vector3(0.0f, 12.0f, -10.0f);
            cameraObject.transform.localRotation = Quaternion.Euler(55.0f, 0.0f, 0.0f);
            Camera mainCamera = cameraObject.AddComponent<Camera>();
            mainCamera.tag = "MainCamera";
            mainCamera.clearFlags = CameraClearFlags.Skybox;
            PlayerCameraFollow cameraFollow = cameraObject.AddComponent<PlayerCameraFollow>();
            cameraFollow.Target = playerAvatar.transform;
            cameraFollow.Offset = new Vector3(0.0f, 12.0f, -10.0f);
            cameraFollow.ApplyFollow();

            GameObject lightObject = new GameObject("Directional Light");
            lightObject.transform.SetParent(root, false);
            lightObject.transform.localRotation = Quaternion.Euler(50.0f, -30.0f, 0.0f);
            Light directionalLight = lightObject.AddComponent<Light>();
            directionalLight.type = LightType.Directional;
            directionalLight.intensity = 1.0f;

            GameObject battleParticipantsObject = new GameObject("BattleParticipants");
            battleParticipantsObject.transform.SetParent(root, false);
            PlayerBattleParticipantRenderer battleParticipantRenderer =
                battleParticipantsObject.AddComponent<PlayerBattleParticipantRenderer>();
            battleParticipantRenderer.NetworkSessionController = networkSessionController;

            GameObject monsterMarkerObject = new GameObject("MonsterMarker");
            monsterMarkerObject.transform.SetParent(root, false);
            PlayerMonsterMarkerRenderer monsterMarkerRenderer =
                monsterMarkerObject.AddComponent<PlayerMonsterMarkerRenderer>();
            monsterMarkerRenderer.NetworkSessionController = networkSessionController;

            GameObject dropMarkersObject = new GameObject("DropMarkers");
            dropMarkersObject.transform.SetParent(root, false);
            PlayerDropMarkerRenderer dropMarkerRenderer =
                dropMarkersObject.AddComponent<PlayerDropMarkerRenderer>();
            dropMarkerRenderer.NetworkSessionController = networkSessionController;

            GameObject inventoryStatusObject = new GameObject("InventoryStatus");
            inventoryStatusObject.transform.SetParent(root, false);
            inventoryStatusObject.transform.localPosition = new Vector3(-8.5f, 4.0f, -6.0f);
            PlayerInventoryStatusRenderer inventoryStatusRenderer =
                inventoryStatusObject.AddComponent<PlayerInventoryStatusRenderer>();
            inventoryStatusRenderer.NetworkSessionController = networkSessionController;

            GameObject lootRaceEvidenceObject = new GameObject("LootRaceEvidence");
            lootRaceEvidenceObject.transform.SetParent(root, false);
            lootRaceEvidenceObject.transform.localPosition =
                new Vector3(-8.5f, 2.1f, -6.0f);
            PlayerLootRaceEvidenceRenderer lootRaceEvidenceRenderer =
                lootRaceEvidenceObject.AddComponent<PlayerLootRaceEvidenceRenderer>();
            lootRaceEvidenceRenderer.NetworkSessionController = networkSessionController;

            return new PlayerClientSceneObjects(
                arenaFloor,
                playerAvatar,
                mainCamera,
                directionalLight,
                playerController,
                cameraFollow,
                networkSessionController,
                battleParticipantRenderer,
                monsterMarkerRenderer,
                dropMarkerRenderer,
                inventoryStatusRenderer,
                lootRaceEvidenceRenderer,
                PrimitiveType.Cube,
                PrimitiveType.Capsule);
        }

    }
}
