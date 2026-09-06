using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace LootOfLegends.Editor
{
    /// <summary>
    /// Renders a presentation Scene to a PNG so layout can be reviewed without a human at the
    /// Editor. Screen space overlay Canvases bypass camera rendering, so each root overlay Canvas
    /// is temporarily retargeted at the capture camera and restored afterwards. The Scene is never
    /// saved, so the captured Scene asset stays byte identical on disk.
    /// </summary>
    public static class SceneCaptureTool
    {
        private const int MaximumLongEdge = 1568;

        // 1536x864 keeps the 16:9 aspect of the 1920x1080 reference resolution while giving the
        // Arena camera an integer 40 pixels per world unit (orthographic size 10.8 spans 21.6
        // units, and 864 / 21.6 == 40). A non integer ratio makes Tilemap cell borders land
        // between screen pixels, which renders as false seams across the background and invites
        // chasing a rendering defect that does not exist at shipping resolutions.
        private const int DefaultWidth = 1536;
        private const int DefaultHeight = 864;

        public static void Capture()
        {
            string scenePath = Argument("--loot-capture-scene=");
            string output = Argument("--loot-capture-output=");
            int width = IntegerArgument("--loot-capture-width=", DefaultWidth);
            int height = IntegerArgument("--loot-capture-height=", DefaultHeight);
            string[] hidden = NameListArgument("--loot-capture-hide=");
            string[] shown = NameListArgument("--loot-capture-show=");
            string[] both = hidden.Intersect(shown).ToArray();
            if (both.Length > 0)
            {
                throw new InvalidOperationException(
                    $"Scene capture cannot hide and show the same GameObject: {string.Join(", ", both)}");
            }

            if (string.IsNullOrWhiteSpace(scenePath) ||
                !scenePath.EndsWith(".unity", StringComparison.Ordinal) ||
                AssetDatabase.LoadAssetAtPath<SceneAsset>(scenePath) == null)
            {
                throw new InvalidOperationException(
                    "Scene capture source must be an existing project relative Scene asset");
            }
            if (string.IsNullOrWhiteSpace(output) || !Path.IsPathRooted(output) ||
                !output.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(
                    "Scene capture output must be an absolute png path");
            }
            if (width < 1 || height < 1 || Math.Max(width, height) > MaximumLongEdge)
            {
                throw new InvalidOperationException(
                    $"Scene capture size must be positive with a long edge of at most {MaximumLongEdge}");
            }

            string parent = Path.GetDirectoryName(output);
            if (string.IsNullOrEmpty(parent))
            {
                throw new InvalidOperationException("Scene capture parent directory is missing");
            }
            Directory.CreateDirectory(parent);

            EditorSceneManager.OpenScene(scenePath, OpenSceneMode.Single);
            Camera camera = ResolveCamera();
            Debug.Log(
                $"SceneCaptureTool scene={scenePath} camera={camera.name} " +
                $"orthographic={camera.orthographic} size={camera.orthographicSize} " +
                $"target={width}x{height}");

            List<OverlayCanvas> restored = new List<OverlayCanvas>();
            List<ToggledObject> toggled = new List<ToggledObject>();
            RenderTexture surface = new RenderTexture(width, height, 24, RenderTextureFormat.Default)
            {
                antiAliasing = 1,
                filterMode = FilterMode.Point
            };
            Texture2D image = null;
            RenderTexture previousActive = RenderTexture.active;
            RenderTexture previousTarget = camera.targetTexture;
            try
            {
                foreach (string name in hidden)
                {
                    toggled.AddRange(ToggledObject.Apply(name, false));
                }
                foreach (string name in shown)
                {
                    toggled.AddRange(ToggledObject.Apply(name, true));
                }
                foreach (Canvas canvas in OverlayCanvases())
                {
                    restored.Add(OverlayCanvas.Retarget(canvas, camera));
                }
                Canvas.ForceUpdateCanvases();

                camera.targetTexture = surface;
                camera.Render();

                RenderTexture.active = surface;
                image = new Texture2D(width, height, TextureFormat.RGB24, false);
                image.ReadPixels(new Rect(0f, 0f, width, height), 0, 0);
                image.Apply();

                File.WriteAllBytes(output, image.EncodeToPNG());
                Debug.Log(
                    $"SceneCaptureTool wrote={output} " +
                    $"retargetedCanvases={restored.Count} " +
                    $"toggledObjects={toggled.Count} " +
                    $"bytes={new FileInfo(output).Length}");
            }
            finally
            {
                RenderTexture.active = previousActive;
                camera.targetTexture = previousTarget;
                foreach (OverlayCanvas canvas in restored)
                {
                    canvas.Restore();
                }
                foreach (ToggledObject target in toggled)
                {
                    target.Restore();
                }
                if (image != null)
                {
                    UnityEngine.Object.DestroyImmediate(image);
                }
                surface.Release();
                UnityEngine.Object.DestroyImmediate(surface);
            }
        }

        private static IEnumerable<Canvas> OverlayCanvases()
        {
            return UnityEngine.Object
                .FindObjectsByType<Canvas>(FindObjectsInactive.Exclude, FindObjectsSortMode.None)
                .Where(canvas => canvas.isRootCanvas)
                .Where(canvas => canvas.renderMode == RenderMode.ScreenSpaceOverlay)
                .OrderBy(canvas => canvas.sortingOrder)
                .ToArray();
        }

        private static Camera ResolveCamera()
        {
            Camera camera = Camera.main ?? UnityEngine.Object
                .FindObjectsByType<Camera>(FindObjectsInactive.Exclude, FindObjectsSortMode.None)
                .OrderBy(candidate => candidate.depth)
                .FirstOrDefault();
            if (camera == null)
            {
                throw new InvalidOperationException("Scene capture requires an enabled Camera");
            }
            return camera;
        }

        private sealed class OverlayCanvas
        {
            private readonly Canvas canvas;
            private readonly RenderMode renderMode;
            private readonly Camera worldCamera;
            private readonly float planeDistance;

            private OverlayCanvas(Canvas canvas)
            {
                this.canvas = canvas;
                renderMode = canvas.renderMode;
                worldCamera = canvas.worldCamera;
                planeDistance = canvas.planeDistance;
            }

            public static OverlayCanvas Retarget(Canvas canvas, Camera camera)
            {
                OverlayCanvas state = new OverlayCanvas(canvas);
                canvas.renderMode = RenderMode.ScreenSpaceCamera;
                canvas.worldCamera = camera;
                canvas.planeDistance = Mathf.Max(camera.nearClipPlane + 0.1f, 1f);
                return state;
            }

            public void Restore()
            {
                canvas.renderMode = renderMode;
                canvas.worldCamera = worldCamera;
                canvas.planeDistance = planeDistance;
            }
        }

        /// <summary>
        /// Forces a GameObject active or inactive for the duration of the capture and restores the
        /// authored state afterwards. Presentation Scenes keep runtime overlays active while
        /// authoring, which hides the content underneath, and keep result screens inactive, which
        /// makes them unreachable from a Scene capture. A name that matches nothing is an error
        /// rather than a silent no-op, because a typo would otherwise be indistinguishable from an
        /// object that is already in the requested state.
        /// </summary>
        private sealed class ToggledObject
        {
            private readonly GameObject target;
            private readonly bool authored;

            private ToggledObject(GameObject target)
            {
                this.target = target;
                authored = target.activeSelf;
            }

            public static IEnumerable<ToggledObject> Apply(string name, bool active)
            {
                GameObject[] matches = UnityEngine.Object
                    .FindObjectsByType<Transform>(FindObjectsInactive.Include, FindObjectsSortMode.None)
                    .Where(transform => transform.name == name)
                    .Select(transform => transform.gameObject)
                    .ToArray();
                if (matches.Length == 0)
                {
                    throw new InvalidOperationException(
                        $"Scene capture cannot toggle '{name}' because the Scene has no such GameObject");
                }

                List<ToggledObject> changed = new List<ToggledObject>();
                foreach (GameObject match in matches.Where(match => match.activeSelf != active))
                {
                    changed.Add(new ToggledObject(match));
                    match.SetActive(active);
                }
                return changed;
            }

            public void Restore()
            {
                target.SetActive(authored);
            }
        }

        private static string[] NameListArgument(string prefix)
        {
            return (Argument(prefix) ?? string.Empty)
                .Split(',')
                .Select(name => name.Trim())
                .Where(name => name.Length > 0)
                .ToArray();
        }

        private static int IntegerArgument(string prefix, int fallback)
        {
            string value = Argument(prefix);
            if (string.IsNullOrWhiteSpace(value))
            {
                return fallback;
            }
            if (!int.TryParse(value, out int parsed))
            {
                throw new InvalidOperationException($"Scene capture argument {prefix} must be an integer");
            }
            return parsed;
        }

        private static string Argument(string prefix)
        {
            string value = Environment.GetCommandLineArgs().FirstOrDefault(
                argument => argument.StartsWith(prefix, StringComparison.Ordinal));
            return value == null ? null : value.Substring(prefix.Length);
        }
    }
}
