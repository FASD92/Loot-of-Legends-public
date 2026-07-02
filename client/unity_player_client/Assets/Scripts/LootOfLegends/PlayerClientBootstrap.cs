using UnityEngine;
using UnityEngine.SceneManagement;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerClientBootstrap : MonoBehaviour
    {
        public const string LoginSceneName = "LoginScene";
        public const string LobbySceneName = "LobbyScene";
        public const string RoomSceneName = "RoomScene";
        public const string ArenaSceneName = "ArenaScene";
        public const string DebugSceneName = "SampleScene";

        private const string RuntimeRootName = "PlayerClientRoot";

        private bool built;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        public static void EnsureBootstrappedForRuntime()
        {
            PlayerClientBootstrap bootstrap =
                Object.FindAnyObjectByType<PlayerClientBootstrap>();
            if (bootstrap == null)
            {
                GameObject root = new GameObject(RuntimeRootName);
                bootstrap = root.AddComponent<PlayerClientBootstrap>();
            }

            bootstrap.BuildIfNeeded();
        }

        private void Awake()
        {
            BuildIfNeeded();
        }

        private void OnEnable()
        {
            SceneManager.sceneLoaded += HandleSceneLoaded;
        }

        private void OnDisable()
        {
            SceneManager.sceneLoaded -= HandleSceneLoaded;
        }

        public void HandleSceneLoadedForTests(Scene scene, LoadSceneMode mode)
        {
            HandleSceneLoaded(scene, mode);
        }

        private void HandleSceneLoaded(Scene scene, LoadSceneMode mode)
        {
            BuildIfNeeded();
        }

        public void BuildIfNeeded()
        {
            EnsureGameSessionRoot();

            if (IsLoginSceneActive())
            {
                ClearBuiltRuntimeChildren();
                return;
            }

            if (IsDebugSceneActive())
            {
                if (!built)
                {
                    PlayerClientSceneFactory.Build(transform);
                    built = true;
                }
                return;
            }

            if (IsArenaSceneActive())
            {
                if (!built)
                {
                    PlayerClientSceneFactory.Build(transform);
                    built = true;
                }
                return;
            }

            ClearBuiltRuntimeChildren();
            EnsureNetworkSessionController();
        }

        private void EnsureGameSessionRoot()
        {
            if (GetComponent<GameSessionRoot>() != null)
            {
                return;
            }

            if (Object.FindAnyObjectByType<GameSessionRoot>() != null)
            {
                return;
            }

            gameObject.AddComponent<GameSessionRoot>();
        }

        private void EnsureNetworkSessionController()
        {
            if (GetComponent<PlayerNetworkSessionController>() != null)
            {
                return;
            }

            if (Object.FindAnyObjectByType<PlayerNetworkSessionController>() != null)
            {
                return;
            }

            gameObject.AddComponent<PlayerNetworkSessionController>();
        }

        private void ClearBuiltRuntimeChildren()
        {
            if (!built && transform.childCount == 0)
            {
                return;
            }

            for (int index = transform.childCount - 1; index >= 0; --index)
            {
                DestroyRuntimeObject(transform.GetChild(index).gameObject);
            }

            built = false;
        }

        private static void DestroyRuntimeObject(GameObject target)
        {
            if (target == null)
            {
                return;
            }

            if (Application.isPlaying)
            {
                Object.Destroy(target);
            }
            else
            {
                Object.DestroyImmediate(target);
            }
        }

        private static bool IsLoginSceneActive()
        {
            return SceneManager.GetActiveScene().name == LoginSceneName;
        }

        private static bool IsDebugSceneActive()
        {
            return SceneManager.GetActiveScene().name == DebugSceneName;
        }

        private static bool IsArenaSceneActive()
        {
            return SceneManager.GetActiveScene().name == ArenaSceneName;
        }
    }
}
