using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Rendering;

namespace LootOfLegends.Tests.Capture
{
    /// <summary>
    /// Renders the running PlayMode Scene to a PNG. The Editor scene capture tool can only show
    /// what authoring leaves in the Scene, which excludes every list row and Arena entity because
    /// those are instantiated from a prefab at runtime. Driving the same views from PlayMode and
    /// capturing the result is the only way to review that content without a server and a build.
    ///
    /// Screen space overlay Canvases bypass camera rendering, so each root overlay Canvas is
    /// retargeted at the capture camera and restored afterwards. Nothing is written back to an
    /// asset, so the Scene stays byte identical on disk.
    /// </summary>
    internal static class RuntimeCapture
    {
        private const string OutputDirectoryArgument = "--loot-capture-dir=";

        // Keeping the long edge at or below 1568 matches the image review pipeline, which
        // downscales anything larger. 1536x864 is 16:9 and gives the Arena camera an integer
        // 40 pixels per world unit (orthographic size 10.8 spans 21.6 units, 864 / 21.6 == 40),
        // so Tilemap cell borders land on screen pixels instead of rendering false seams.
        public const int DefaultWidth = 1536;
        public const int DefaultHeight = 864;

        private const int MaximumLongEdge = 1568;

        /// <summary>
        /// Absolute directory the capture writes into, or null when the run did not ask for a
        /// capture. Scenarios ignore themselves in that case, which is what keeps an ordinary
        /// PlayMode test run from writing images as a side effect.
        /// </summary>
        public static string RequestedOutputDirectory()
        {
            string value = Environment.GetCommandLineArgs().FirstOrDefault(
                argument => argument.StartsWith(OutputDirectoryArgument, StringComparison.Ordinal));
            if (value == null)
            {
                return null;
            }
            string directory = value.Substring(OutputDirectoryArgument.Length).Trim();
            if (directory.Length == 0 || !Path.IsPathRooted(directory))
            {
                throw new InvalidOperationException(
                    OutputDirectoryArgument + " must be an absolute directory path");
            }
            return directory;
        }

        /// <summary>
        /// Skips the calling scenario unless the run asked for a capture and can actually
        /// rasterise one. A headless graphics device silently produces a blank image, which is
        /// worse than no image because it looks like a rendering defect.
        /// </summary>
        public static string RequireOutputDirectory()
        {
            string directory = RequestedOutputDirectory();
            if (directory == null)
            {
                Assert.Ignore(
                    "Scene capture scenario runs only when " + OutputDirectoryArgument +
                    " names an absolute output directory");
            }
            if (SystemInfo.graphicsDeviceType == GraphicsDeviceType.Null)
            {
                throw new InvalidOperationException(
                    "Scene capture needs a real graphics device; a null device renders a blank " +
                    "image, so the runner must not pass -nographics");
            }
            Directory.CreateDirectory(directory);
            return directory;
        }

        public static IEnumerator Write(string fileName)
        {
            return Write(fileName, DefaultWidth, DefaultHeight);
        }

        public static IEnumerator Write(string fileName, int width, int height)
        {
            if (string.IsNullOrWhiteSpace(fileName) ||
                !fileName.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException("Scene capture file name must end in .png");
            }
            if (width < 1 || height < 1 || Math.Max(width, height) > MaximumLongEdge)
            {
                throw new InvalidOperationException(
                    "Scene capture size must be positive with a long edge of at most " +
                    MaximumLongEdge);
            }

            string output = Path.Combine(RequireOutputDirectory(), fileName);
            Camera camera = ResolveCamera();
            var restored = new List<OverlayCanvas>();
            var surface = new RenderTexture(width, height, 24, RenderTextureFormat.Default)
            {
                antiAliasing = 1,
                filterMode = FilterMode.Point
            };
            Texture2D image = null;
            RenderTexture previousActive = RenderTexture.active;
            RenderTexture previousTarget = camera.targetTexture;
            try
            {
                foreach (Canvas canvas in OverlayCanvases())
                {
                    restored.Add(OverlayCanvas.Retarget(canvas, camera));
                }

                // The target texture has to be attached before the layout settles: a
                // CanvasScaler reads Canvas.renderingDisplaySize, which follows the camera's
                // pixel rect, and it only recomputes on its own Update. Two frames cover the
                // scaler pass and the graphic rebuild it triggers.
                camera.targetTexture = surface;
                yield return null;
                yield return null;
                Canvas.ForceUpdateCanvases();

                camera.Render();

                RenderTexture.active = surface;
                image = new Texture2D(width, height, TextureFormat.RGB24, false);
                image.ReadPixels(new Rect(0f, 0f, width, height), 0, 0);
                image.Apply();
                File.WriteAllBytes(output, image.EncodeToPNG());

                Debug.Log(
                    $"RuntimeCapture wrote={output} camera={camera.name} " +
                    $"target={width}x{height} " +
                    $"retargetedCanvases={restored.Count} " +
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
                if (image != null)
                {
                    UnityEngine.Object.Destroy(image);
                }
                surface.Release();
                UnityEngine.Object.Destroy(surface);
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
            /// <summary>
            /// A screen space overlay Canvas always draws after everything else, but a retargeted
            /// Canvas sorts against SpriteRenderers by sorting order instead. Without a lift the
            /// Arena players and monster punch through every panel drawn over them, which reads as
            /// a z-order defect that does not exist at runtime. The original order is added back so
            /// Canvases keep their relative stacking.
            /// </summary>
            private const int SortingOrderLift = 1000;

            private readonly Canvas canvas;
            private readonly RenderMode renderMode;
            private readonly Camera worldCamera;
            private readonly float planeDistance;
            private readonly int sortingOrder;
            private readonly bool overrideSorting;

            private OverlayCanvas(Canvas canvas)
            {
                this.canvas = canvas;
                renderMode = canvas.renderMode;
                worldCamera = canvas.worldCamera;
                planeDistance = canvas.planeDistance;
                sortingOrder = canvas.sortingOrder;
                overrideSorting = canvas.overrideSorting;
            }

            public static OverlayCanvas Retarget(Canvas canvas, Camera camera)
            {
                var state = new OverlayCanvas(canvas);
                canvas.renderMode = RenderMode.ScreenSpaceCamera;
                canvas.worldCamera = camera;
                canvas.planeDistance = Mathf.Max(camera.nearClipPlane + 0.1f, 1f);
                canvas.overrideSorting = true;
                canvas.sortingOrder = SortingOrderLift + state.sortingOrder;
                return state;
            }

            public void Restore()
            {
                canvas.renderMode = renderMode;
                canvas.worldCamera = worldCamera;
                canvas.planeDistance = planeDistance;
                canvas.sortingOrder = sortingOrder;
                canvas.overrideSorting = overrideSorting;
            }
        }
    }
}
