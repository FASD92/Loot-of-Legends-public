using System.Threading.Tasks;
using LootOfLegends.PlayerClient;
using NUnit.Framework;
using UnityEngine;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerNetworkSessionControllerTests
    {
        [Test]
        public void EndpointUsesDefaultServerPorts()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                Assert.AreEqual("127.0.0.1", controller.Endpoint.Host);
                Assert.AreEqual(40000, controller.Endpoint.TcpPort);
                Assert.AreEqual(40000, controller.Endpoint.RudpPort);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
                Assert.AreEqual(0UL, controller.SessionId);
                Assert.IsFalse(controller.RudpHelloSent);
                Assert.AreEqual(0UL, controller.RudpHelloSessionId);
                Assert.AreEqual(0U, controller.RudpHelloSequence);
                Assert.AreEqual(0, controller.ClientSessionCount);
                Assert.IsFalse(controller.SelfListedInClientListSnapshot);
                Assert.AreEqual(new ulong[0], controller.ClientSessionIds);
                Assert.AreEqual(0U, controller.CurrentRoomId);
                Assert.AreEqual(0, controller.CurrentRoomPlayerCount);
                Assert.AreEqual(0, controller.CurrentRoomReadyPlayerCount);
                Assert.IsFalse(controller.BattleStarted);
                Assert.AreEqual(0U, controller.BattleStartRoomId);
                Assert.AreEqual(0UL, controller.BattleStartPlayerASessionId);
                Assert.AreEqual(0UL, controller.BattleStartPlayerBSessionId);
                Assert.IsFalse(controller.MonsterSpawned);
                Assert.AreEqual(0U, controller.MonsterSpawnRoomId);
                Assert.AreEqual(0U, controller.MonsterId);
                Assert.AreEqual(0U, controller.MonsterTypeId);
                Assert.AreEqual(0, controller.MonsterMaxHp);
                Assert.IsFalse(controller.RudpAttackSent);
                Assert.AreEqual(0UL, controller.RudpAttackSessionId);
                Assert.AreEqual(0U, controller.RudpAttackSequence);
                Assert.AreEqual(0U, controller.RudpAttackCommandSequence);
                Assert.AreEqual(0U, controller.RudpAttackTargetHintMonsterId);
                Assert.AreEqual(string.Empty, controller.RudpAttackLocalEndpoint);
                Assert.IsFalse(controller.RudpSpaceLootSent);
                Assert.AreEqual(0UL, controller.RudpSpaceLootSessionId);
                Assert.AreEqual(0U, controller.RudpSpaceLootSequence);
                Assert.AreEqual(0U, controller.RudpSpaceLootCommandSequence);
                Assert.AreEqual(string.Empty, controller.RudpSpaceLootLocalEndpoint);
                Assert.IsFalse(controller.MonsterHealthSnapshotCaptured);
                Assert.AreEqual(0U, controller.MonsterHealthRoomId);
                Assert.AreEqual(0U, controller.MonsterHealthMonsterId);
                Assert.AreEqual(0, controller.MonsterCurrentHp);
                Assert.AreEqual(0, controller.MonsterHealthMaxHp);
                Assert.IsFalse(controller.MonsterDeathCaptured);
                Assert.AreEqual(0U, controller.MonsterDeathRoomId);
                Assert.AreEqual(0U, controller.MonsterDeathMonsterId);
                Assert.IsFalse(controller.DropListSnapshotV2Captured);
                Assert.AreEqual(0U, controller.DropListSnapshotV2RoomId);
                Assert.AreEqual(0U, controller.DropListSnapshotV2ScatterSeed);
                Assert.AreEqual(0, controller.DropListSnapshotV2DropCount);
                Assert.AreEqual(new PlayerDropEntryV2[0], controller.DropListSnapshotV2Drops);
                Assert.IsFalse(controller.LootResolvedCaptured);
                Assert.AreEqual(0U, controller.LootResolved.RoomId);
                Assert.AreEqual(0U, controller.LootResolved.DropId);
                Assert.AreEqual(0UL, controller.LootResolved.WinnerSessionId);
                Assert.AreEqual(0U, controller.LootResolved.ItemId);
                Assert.AreEqual(0, controller.LootResolved.Quantity);
                Assert.IsFalse(controller.LootRejectedCaptured);
                Assert.AreEqual(0U, controller.LootRejected.RoomId);
                Assert.AreEqual(0U, controller.LootRejected.DropId);
                Assert.AreEqual(PlayerLootRejectReason.None, controller.LootRejected.Reason);
                Assert.IsFalse(controller.InventorySnapshotCaptured);
                Assert.AreEqual(0UL, controller.InventorySnapshot.SessionId);
                Assert.AreEqual(0, controller.InventorySnapshot.CurrentWeight);
                Assert.AreEqual(0, controller.InventorySnapshot.MaxWeight);
                Assert.AreEqual(0, controller.InventorySnapshot.Count);
                Assert.AreEqual(new PlayerInventoryEntry[0], controller.InventorySnapshotEntries);
                Assert.AreEqual(0, controller.RoomListCount);
                Assert.AreEqual(new PlayerRoomListEntry[0], controller.RoomListEntries);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task RequestCreateRoomAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool sent = await controller.RequestCreateRoomAsync();

                Assert.IsFalse(sent);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task RequestJoinRoomAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool sent = await controller.RequestJoinRoomAsync(17U);

                Assert.IsFalse(sent);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task RequestReadyRoomAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.RequestReadyRoomAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task RequestLeaveRoomAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.RequestLeaveRoomAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task CaptureMonsterSpawnAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.CaptureMonsterSpawnAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task SendAttackIntentAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool sent = await controller.SendAttackIntentAsync();

                Assert.IsFalse(sent);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task SendSpaceLootIntentAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool sent = await controller.SendSpaceLootIntentAsync();

                Assert.IsFalse(sent);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task CaptureMonsterHealthSnapshotAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.CaptureMonsterHealthSnapshotAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task CaptureMonsterDeathAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.CaptureMonsterDeathAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task CaptureDropListSnapshotV2AsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.CaptureDropListSnapshotV2Async();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task CaptureLootResolvedAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.CaptureLootResolvedAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
                Assert.IsFalse(controller.LootResolvedCaptured);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task CaptureLootRejectedAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.CaptureLootRejectedAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
                Assert.IsFalse(controller.LootRejectedCaptured);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task CaptureInventorySnapshotAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.CaptureInventorySnapshotAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
                Assert.IsFalse(controller.InventorySnapshotCaptured);
                Assert.AreEqual(0, controller.InventorySnapshot.Count);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }

        [Test]
        public async Task CaptureBattleStartAsyncReturnsFalseBeforeConnect()
        {
            GameObject host = new GameObject("NetworkControllerHost");
            try
            {
                PlayerNetworkSessionController controller = host.AddComponent<PlayerNetworkSessionController>();

                bool captured = await controller.CaptureBattleStartAsync();

                Assert.IsFalse(captured);
                Assert.AreEqual(PlayerNetworkSessionStatus.Disconnected, controller.Status);
            }
            finally
            {
                Object.DestroyImmediate(host);
            }
        }
    }
}
