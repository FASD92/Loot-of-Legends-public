using System;
using System.Linq;
using UnityEditor;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Editor
{
    /// <summary>
    /// Gives the SafeFailure notice a dim layer so the cream Kenney dialog separates from whatever
    /// screen is behind it.
    ///
    /// Without a dim the dialog is cream on top of the equally cream Lobby or Room panel, so only
    /// its thin border distinguishes it and it reads as a box drawn inside that panel instead of a
    /// modal above it. FinalResultPanel already establishes the convention this mirrors: a
    /// sprite-less black layer at 0.72 alpha behind a Kenney skinned modal.
    ///
    /// This runs as a builder rather than as hand written prefab YAML because the change reparents
    /// a nested Prefab instance and rewires a private SerializeField. Both carry bookkeeping that
    /// the engine owns: stripped instance entries, child ordering, and the reference itself. A hand
    /// edit that gets any of it subtly wrong yields an overlay that silently never appears.
    ///
    /// Idempotent. The dim layer is looked up by name and only created when missing, and
    /// SaveAsPrefabAsset preserves the fileID of every object that already exists, so the
    /// panel and label bindings that four Scenes rely on survive a re-run.
    /// </summary>
    public static class SafeFailureOverlayBuilder
    {
        private const string OverlayPrefab =
            "Assets/Presentation/Prefabs/Ugui/SafeFailureOverlay.prefab";

        private const string DimLayerName = "SafeFailureDimLayer";
        private const string BlockingPanelName = "BlockingPanel";

        // FinalResultDimLayer 와 같은 값. 두 화면의 dim 이 갈리면 같은 게임으로 보이지 않는다.
        private static readonly Color DimColour = new Color(0f, 0f, 0f, 0.72156864f);

        public static void Build()
        {
            GameObject contents = PrefabUtility.LoadPrefabContents(OverlayPrefab);
            try
            {
                Transform blocking = contents.transform
                    .GetComponentsInChildren<Transform>(true)
                    .FirstOrDefault(child => child.name == BlockingPanelName)
                    ?? throw new InvalidOperationException(
                        $"{OverlayPrefab} must contain a {BlockingPanelName}");

                RectTransform dim = EnsureDimLayer(contents);

                // 다이얼로그를 dim 안으로 옮긴다. 이미 안에 있으면 SetParent 가 아무 일도 하지
                // 않으므로 재실행이 안전하다. worldPositionStays=false 라야 앵커 기준 배치가
                // 유지된다.
                if (blocking.parent != dim)
                {
                    blocking.SetParent(dim, false);
                    Debug.Log(
                        $"SafeFailureOverlayBuilder reparented {BlockingPanelName} " +
                        $"under {DimLayerName}");
                }

                // 토글 대상이 dim 으로 바뀌므로 다이얼로그 자체는 항상 켜져 있어야 한다.
                // 둘 다 꺼두면 dim 만 켜지고 알림이 보이지 않는다.
                if (!blocking.gameObject.activeSelf)
                {
                    blocking.gameObject.SetActive(true);
                    Debug.Log($"SafeFailureOverlayBuilder activated {BlockingPanelName}");
                }
                if (dim.gameObject.activeSelf)
                {
                    // 저작 상태는 숨김이다. SafeFailureTextView.Awake 가 같은 상태로 시작한다.
                    dim.gameObject.SetActive(false);
                    Debug.Log($"SafeFailureOverlayBuilder hid {DimLayerName} for authoring");
                }

                RebindPanel(contents, dim.gameObject);

                PrefabUtility.SaveAsPrefabAsset(contents, OverlayPrefab);
                Debug.Log($"SafeFailureOverlayBuilder saved {OverlayPrefab}");
            }
            finally
            {
                PrefabUtility.UnloadPrefabContents(contents);
            }

            AssetDatabase.SaveAssets();
            AssetDatabase.Refresh();
        }

        private static RectTransform EnsureDimLayer(GameObject contents)
        {
            Transform existing = contents.transform
                .GetComponentsInChildren<Transform>(true)
                .FirstOrDefault(child => child.name == DimLayerName);
            if (existing != null)
            {
                return Stretch((RectTransform)existing);
            }

            var host = new GameObject(DimLayerName, typeof(RectTransform));
            host.layer = contents.layer;
            host.transform.SetParent(contents.transform, false);
            Debug.Log($"SafeFailureOverlayBuilder created {DimLayerName}");

            var image = host.AddComponent<Image>();

            // 스프라이트 없는 단색이다. Kenney 프레임을 여기 붙이면 화면 테두리가 되어
            // 다이얼로그의 프레임과 경쟁한다.
            image.sprite = null;
            image.color = DimColour;
            image.raycastTarget = true;

            return Stretch(host.GetComponent<RectTransform>());
        }

        private static RectTransform Stretch(RectTransform rect)
        {
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.pivot = new Vector2(0.5f, 0.5f);
            rect.offsetMin = Vector2.zero;
            rect.offsetMax = Vector2.zero;
            rect.localScale = Vector3.one;
            return rect;
        }

        private static void RebindPanel(GameObject contents, GameObject dim)
        {
            Component view = contents
                .GetComponents<Component>()
                .FirstOrDefault(component =>
                    component != null &&
                    component.GetType().Name == "SafeFailureTextView")
                ?? throw new InvalidOperationException(
                    $"{OverlayPrefab} must carry a SafeFailureTextView");

            var serialized = new SerializedObject(view);
            SerializedProperty panel = serialized.FindProperty("panel")
                ?? throw new InvalidOperationException(
                    "SafeFailureTextView must expose a panel field");
            SerializedProperty label = serialized.FindProperty("label")
                ?? throw new InvalidOperationException(
                    "SafeFailureTextView must expose a label field");

            if (panel.objectReferenceValue != (object)dim)
            {
                panel.objectReferenceValue = dim;
                serialized.ApplyModifiedPropertiesWithoutUndo();
                Debug.Log($"SafeFailureOverlayBuilder repointed panel to {DimLayerName}");
            }

            // 메시지 라벨은 이 빌더의 관심사가 아니지만, 끊긴 채로 저장하면 네 씬의 바인딩
            // 테스트가 통과하는 동안에도 런타임에 알림이 비어 나온다.
            if (label.objectReferenceValue == null)
            {
                throw new InvalidOperationException(
                    "SafeFailureTextView label binding was lost");
            }
        }
    }
}
