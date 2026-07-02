using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerMonsterHealthBarTests
    {
        [Test]
        public void RenderCreatesBackgroundAndFillChildren()
        {
            GameObject root = new GameObject("HealthBarRoot");
            try
            {
                PlayerMonsterHealthBar bar =
                    root.AddComponent<PlayerMonsterHealthBar>();

                bar.Render(75, 100);

                Assert.NotNull(bar.Background);
                Assert.NotNull(bar.Fill);
                Assert.AreSame(root.transform, bar.Background.transform.parent);
                Assert.AreSame(root.transform, bar.Fill.transform.parent);
                Assert.IsNull(bar.Background.GetComponent<Collider>());
                Assert.IsNull(bar.Fill.GetComponent<Collider>());
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderScalesFillToCurrentHpRatio()
        {
            GameObject root = new GameObject("HealthBarRoot");
            try
            {
                PlayerMonsterHealthBar bar =
                    root.AddComponent<PlayerMonsterHealthBar>();

                bar.Render(25, 100);

                Assert.AreEqual(0.25f, bar.FillRatio, 0.0001f);
                Assert.AreEqual(0.25f, bar.Fill.transform.localScale.x, 0.0001f);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }
    }
}
