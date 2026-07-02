using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    internal static class Release0VisualMaterialUtility
    {
        public static void ApplyFallbackMaterialIfRootRenderer(
            GameObject target,
            string materialName,
            Color color,
            string materialResourcePath = null)
        {
            Renderer rootRenderer = target.GetComponent<Renderer>();
            if (rootRenderer == null)
            {
                return;
            }

            Renderer[] renderers = target.GetComponentsInChildren<Renderer>(true);
            if (renderers.Length != 1 || renderers[0] != rootRenderer)
            {
                return;
            }

            Material material = CreateMaterial(materialResourcePath);
            if (material == null)
            {
                return;
            }

            material.name = materialName;
            ApplyMaterialColor(material, color);
            rootRenderer.sharedMaterial = material;
        }

        public static GameObject CreateIdentityDisc(
            Transform parent,
            string name,
            Color color,
            Vector3 localPosition,
            float diameter)
        {
            GameObject disc = GameObject.CreatePrimitive(PrimitiveType.Cylinder);
            disc.name = name;
            disc.transform.SetParent(parent, false);
            disc.transform.localPosition = localPosition;
            disc.transform.localRotation = Quaternion.identity;
            disc.transform.localScale = new Vector3(diameter, 0.02f, diameter);
            RemoveRuntimeColliders(disc);

            Renderer renderer = disc.GetComponent<Renderer>();
            if (renderer != null)
            {
                Material material = CreateMaterial(null);
                if (material != null)
                {
                    material.name = $"{name}Material";
                    ApplyMaterialColor(material, color);
                    renderer.sharedMaterial = material;
                }
            }

            return disc;
        }

        public static void RemoveRuntimeColliders(GameObject target)
        {
            Collider[] colliders = target.GetComponentsInChildren<Collider>(true);
            for (int index = 0; index < colliders.Length; ++index)
            {
                Collider collider = colliders[index];
                if (Application.isPlaying)
                {
                    Object.Destroy(collider);
                }
                else
                {
                    Object.DestroyImmediate(collider);
                }
            }
        }

        private static Material CreateMaterial(string materialResourcePath)
        {
            if (!string.IsNullOrEmpty(materialResourcePath))
            {
                Material materialAsset = Resources.Load<Material>(materialResourcePath);
                if (materialAsset != null)
                {
                    return new Material(materialAsset);
                }
            }

            Shader shader = FindSceneShader();
            return shader != null ? new Material(shader) : null;
        }

        private static void ApplyMaterialColor(Material material, Color color)
        {
            if (material.HasProperty("_BaseColor"))
            {
                material.SetColor("_BaseColor", color);
            }
            if (material.HasProperty("_Color"))
            {
                material.SetColor("_Color", color);
            }
        }

        private static Shader FindSceneShader()
        {
            Shader shader = Shader.Find("Universal Render Pipeline/Lit");
            if (shader != null)
            {
                return shader;
            }

            shader = Shader.Find("Universal Render Pipeline/Unlit");
            return shader != null ? shader : Shader.Find("Standard");
        }
    }
}
