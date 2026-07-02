using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public interface IRelease0VisualProvider
    {
        GameObject CreateArenaFloor();
        GameObject CreateLocalPlayer();
        GameObject CreateRemotePlayer();
        GameObject CreateMonster();
        GameObject CreateDrop();
    }

    public static class Release0VisualProviders
    {
        private static IRelease0VisualProvider current =
            new Release0ResourcesVisualProvider();

        public static IRelease0VisualProvider Current => current;

        public static void Use(IRelease0VisualProvider provider)
        {
            current = provider ?? new Release0ResourcesVisualProvider();
        }

        public static void Reset()
        {
            current = new Release0ResourcesVisualProvider();
        }
    }

    public sealed class Release0ResourcesVisualProvider : IRelease0VisualProvider
    {
        public const string DefaultArenaFloorPath = "Release0Visuals/ArenaFloor";
        public const string DefaultLocalPlayerPath = "Release0Visuals/LocalPlayer";
        public const string DefaultRemotePlayerPath = "Release0Visuals/RemotePlayer";
        public const string DefaultMonsterPath = "Release0Visuals/Monster";
        public const string DefaultDropPath = "Release0Visuals/Drop";

        private readonly string arenaFloorPath;
        private readonly string localPlayerPath;
        private readonly string remotePlayerPath;
        private readonly string monsterPath;
        private readonly string dropPath;

        public Release0ResourcesVisualProvider()
            : this(
                DefaultArenaFloorPath,
                DefaultLocalPlayerPath,
                DefaultRemotePlayerPath,
                DefaultMonsterPath,
                DefaultDropPath)
        {
        }

        public Release0ResourcesVisualProvider(
            string arenaFloorPath,
            string localPlayerPath,
            string remotePlayerPath,
            string monsterPath,
            string dropPath)
        {
            this.arenaFloorPath = arenaFloorPath;
            this.localPlayerPath = localPlayerPath;
            this.remotePlayerPath = remotePlayerPath;
            this.monsterPath = monsterPath;
            this.dropPath = dropPath;
        }

        public GameObject CreateArenaFloor()
        {
            return InstantiateResourceOrPrimitive(
                arenaFloorPath,
                PrimitiveType.Cube,
                "ArenaFloor");
        }

        public GameObject CreateLocalPlayer()
        {
            return InstantiateResourceOrPrimitive(
                localPlayerPath,
                PrimitiveType.Capsule,
                "PlayerAvatar");
        }

        public GameObject CreateRemotePlayer()
        {
            return InstantiateResourceOrPrimitive(
                remotePlayerPath,
                PrimitiveType.Capsule,
                "RemoteParticipant");
        }

        public GameObject CreateMonster()
        {
            return InstantiateResourceOrPrimitive(
                monsterPath,
                PrimitiveType.Sphere,
                "Monster");
        }

        public GameObject CreateDrop()
        {
            return InstantiateResourceOrPrimitive(
                dropPath,
                PrimitiveType.Sphere,
                "DropMarker");
        }

        private static GameObject InstantiateResourceOrPrimitive(
            string resourcePath,
            PrimitiveType fallbackPrimitive,
            string objectName)
        {
            GameObject prefab = Resources.Load<GameObject>(resourcePath);
            GameObject visual = prefab != null ?
                Object.Instantiate(prefab) :
                GameObject.CreatePrimitive(fallbackPrimitive);
            visual.name = objectName;
            return visual;
        }
    }
}
