using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerMonsterHealthBar : MonoBehaviour
    {
        private static readonly Color BackgroundColor =
            new Color(0.12f, 0.12f, 0.12f, 1.0f);
        private static readonly Color FillColor =
            new Color(0.86f, 0.12f, 0.12f, 1.0f);

        private GameObject background;
        private GameObject fill;
        private float fillRatio;

        public GameObject Background => background;

        public GameObject Fill => fill;

        public float FillRatio => fillRatio;

        public void Render(ushort currentHp, ushort maxHp)
        {
            EnsureBarObjects();
            fillRatio = maxHp == 0 ? 0.0f : Mathf.Clamp01((float)currentHp / maxHp);
            background.transform.localPosition = Vector3.zero;
            background.transform.localScale = new Vector3(1.0f, 0.08f, 0.08f);
            fill.transform.localPosition =
                new Vector3((fillRatio - 1.0f) * 0.5f, 0.01f, 0.0f);
            fill.transform.localScale = new Vector3(fillRatio, 0.09f, 0.09f);
        }

        private void EnsureBarObjects()
        {
            if (background == null)
            {
                background = CreateBarPart("MonsterHealthBarBackground", BackgroundColor);
            }

            if (fill == null)
            {
                fill = CreateBarPart("MonsterHealthBarFill", FillColor);
            }
        }

        private GameObject CreateBarPart(string objectName, Color color)
        {
            GameObject part = GameObject.CreatePrimitive(PrimitiveType.Cube);
            part.name = objectName;
            part.transform.SetParent(transform, false);
            RemoveRuntimeColliders(part);

            Renderer renderer = part.GetComponent<Renderer>();
            if (renderer != null)
            {
                renderer.sharedMaterial = CreateMaterial(color);
            }

            return part;
        }

        private static Material CreateMaterial(Color color)
        {
            Shader shader = Shader.Find("Universal Render Pipeline/Unlit");
            if (shader == null)
            {
                shader = Shader.Find("Standard");
            }

            if (shader == null)
            {
                return null;
            }

            Material material = new Material(shader);
            if (material.HasProperty("_BaseColor"))
            {
                material.SetColor("_BaseColor", color);
            }
            if (material.HasProperty("_Color"))
            {
                material.SetColor("_Color", color);
            }

            return material;
        }

        private static void RemoveRuntimeColliders(GameObject target)
        {
            Collider[] colliders = target.GetComponentsInChildren<Collider>(true);
            for (int index = 0; index < colliders.Length; ++index)
            {
                Collider collider = colliders[index];
                if (Application.isPlaying)
                {
                    Destroy(collider);
                }
                else
                {
                    DestroyImmediate(collider);
                }
            }
        }
    }
}
