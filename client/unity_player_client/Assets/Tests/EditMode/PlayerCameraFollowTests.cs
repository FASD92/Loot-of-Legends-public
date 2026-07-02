using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerCameraFollowTests
    {
        [Test]
        public void ApplyFollowPlacesCameraAtTargetOffset()
        {
            GameObject target = new GameObject("Target");
            GameObject cameraObject = new GameObject("Camera");
            try
            {
                target.transform.position = new Vector3(2.0f, 1.0f, -3.0f);
                PlayerCameraFollow follow = cameraObject.AddComponent<PlayerCameraFollow>();
                follow.Target = target.transform;
                follow.Offset = new Vector3(0.0f, 12.0f, -10.0f);

                follow.ApplyFollow();

                Assert.AreEqual(2.0f, cameraObject.transform.position.x, 0.0001f);
                Assert.AreEqual(13.0f, cameraObject.transform.position.y, 0.0001f);
                Assert.AreEqual(-13.0f, cameraObject.transform.position.z, 0.0001f);

                Vector3 directionToTarget = (target.transform.position - cameraObject.transform.position).normalized;
                Assert.Greater(Vector3.Dot(cameraObject.transform.forward, directionToTarget), 0.99f);
            }
            finally
            {
                Object.DestroyImmediate(target);
                Object.DestroyImmediate(cameraObject);
            }
        }
    }
}
