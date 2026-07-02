using LootOfLegends.PlayerClient;
using NUnit.Framework;
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using UnityEngine;
using Object = UnityEngine.Object;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerDropMarkerRendererTests
    {
        [Test]
        public void RenderDropsCreatesMarkersAtDisplayScaledPositions()
        {
            GameObject root = new GameObject("DropRoot");
            try
            {
                PlayerDropMarkerRenderer renderer = root.AddComponent<PlayerDropMarkerRenderer>();

                renderer.RenderDrops(new[]
                {
                    new PlayerDropEntryV2(10U, 1001U, 1, -1000, 2500),
                    new PlayerDropEntryV2(11U, 1002U, 2, 3000, -4000)
                });

                Assert.AreEqual(2, renderer.MarkerCount);
                GameObject first = renderer.GetMarker(10U);
                GameObject second = renderer.GetMarker(11U);
                Assert.NotNull(first);
                Assert.NotNull(second);
                Assert.AreEqual("DropMarker_10", first.name);
                Assert.AreSame(root.transform, first.transform.parent);
                Assert.AreEqual(
                    1.0f / PlayerRudpStateSnapshotPlayer.PositionScale,
                    PlayerDropMarkerRenderer.FixedToUnityDisplayScale,
                    0.0000001f);
                Assert.AreEqual(-1.0f, first.transform.localPosition.x, 0.0001f);
                Assert.AreEqual(
                    PlayerDropMarkerRenderer.DefaultMarkerHeight,
                    first.transform.localPosition.y,
                    0.0001f);
                Assert.AreEqual(
                    0.65f,
                    PlayerDropMarkerRenderer.DefaultMarkerHeight,
                    0.0001f);
                Assert.AreEqual(
                    0.75f,
                    PlayerDropMarkerRenderer.DefaultMarkerScale,
                    0.0001f);
                Assert.AreEqual(
                    Vector3.one * PlayerDropMarkerRenderer.DefaultMarkerScale,
                    first.transform.localScale);
                Assert.AreEqual(2.5f, first.transform.localPosition.z, 0.0001f);
                Assert.AreEqual(3.0f, second.transform.localPosition.x, 0.0001f);
                Assert.AreEqual(-4.0f, second.transform.localPosition.z, 0.0001f);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderDropsRemovesStaleMarkersAndClearRemovesAll()
        {
            GameObject root = new GameObject("DropRoot");
            try
            {
                PlayerDropMarkerRenderer renderer = root.AddComponent<PlayerDropMarkerRenderer>();
                renderer.RenderDrops(new[]
                {
                    new PlayerDropEntryV2(10U, 1001U, 1, 0, 0),
                    new PlayerDropEntryV2(11U, 1002U, 1, 1000, 0)
                });

                renderer.RenderDrops(new[]
                {
                    new PlayerDropEntryV2(11U, 1002U, 1, 2000, 0)
                });

                Assert.AreEqual(1, renderer.MarkerCount);
                Assert.IsNull(renderer.GetMarker(10U));
                Assert.NotNull(renderer.GetMarker(11U));
                Assert.AreEqual(
                    2.0f,
                    renderer.GetMarker(11U).transform.localPosition.x,
                    0.0001f);

                renderer.RenderDrops(null);

                Assert.AreEqual(0, renderer.MarkerCount);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void RenderDropsUsesRelease0VisualProviderForGenericDropMarker()
        {
            FakeVisualProvider fake = new FakeVisualProvider();
            Release0VisualProviders.Use(fake);
            GameObject root = new GameObject("DropRoot");
            try
            {
                PlayerDropMarkerRenderer renderer = root.AddComponent<PlayerDropMarkerRenderer>();

                renderer.RenderDrops(new[]
                {
                    new PlayerDropEntryV2(10U, 1001U, 1, 0, 0)
                });

                GameObject marker = renderer.GetMarker(10U);
                Assert.AreEqual(1, fake.DropCallCount);
                Assert.NotNull(marker);
                Assert.AreEqual("DropMarker_10", marker.name);
                Assert.IsNull(marker.GetComponent<Collider>());
                Assert.IsNull(marker.GetComponentInChildren<Collider>());
                AssertChildRendererUsesMaterial(
                    marker,
                    "FakeDropModel",
                    fake.DropMaterial);
                AssertIdentityMarkerColor(
                    marker,
                    "DropIdentityMarker",
                    new Color(0.95f, 0.84f, 0.22f, 1.0f));
                AssertIdentityMarkerScale(
                    marker,
                    "DropIdentityMarker",
                    PlayerDropMarkerRenderer.IdentityDiscScale);
                Assert.AreEqual(
                    0.20f,
                    PlayerDropMarkerRenderer.IdentityDiscWorldDiameter,
                    0.0001f);
                AssertIdentityMarkerWorldPose(
                    marker,
                    "DropIdentityMarker",
                    PlayerDropMarkerRenderer.IdentityDiscWorldDiameter);
            }
            finally
            {
                Object.DestroyImmediate(root);
                Release0VisualProviders.Reset();
            }
        }

        [Test]
        public void ApplyLootResolvedRemovesClaimedMarkerOnly()
        {
            GameObject root = new GameObject("DropRoot");
            try
            {
                PlayerDropMarkerRenderer renderer = root.AddComponent<PlayerDropMarkerRenderer>();
                renderer.RenderDrops(new[]
                {
                    new PlayerDropEntryV2(10U, 1001U, 1, 0, 0),
                    new PlayerDropEntryV2(11U, 1002U, 1, 1000, 0)
                });

                bool removed = renderer.ApplyLootResolved(
                    new PlayerLootResolved(1U, 10U, 99UL, 1001U, 1));

                Assert.IsTrue(removed);
                Assert.AreEqual(1, renderer.MarkerCount);
                Assert.IsNull(renderer.GetMarker(10U));
                Assert.NotNull(renderer.GetMarker(11U));
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void ApplyLootResolvedReturnsFalseForUnknownOrZeroDrop()
        {
            GameObject root = new GameObject("DropRoot");
            try
            {
                PlayerDropMarkerRenderer renderer = root.AddComponent<PlayerDropMarkerRenderer>();
                renderer.RenderDrops(new[]
                {
                    new PlayerDropEntryV2(10U, 1001U, 1, 0, 0)
                });

                Assert.IsFalse(renderer.ApplyLootResolved(
                    new PlayerLootResolved(1U, 0U, 99UL, 1001U, 1)));
                Assert.IsFalse(renderer.ApplyLootResolved(
                    new PlayerLootResolved(1U, 99U, 99UL, 1001U, 1)));
                Assert.AreEqual(1, renderer.MarkerCount);
                Assert.NotNull(renderer.GetMarker(10U));
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public void ApplyCapturedLootResolvedReturnsFalseWithoutCapturedResult()
        {
            GameObject root = new GameObject("DropRoot");
            try
            {
                PlayerDropMarkerRenderer renderer = root.AddComponent<PlayerDropMarkerRenderer>();
                renderer.RenderDrops(new[]
                {
                    new PlayerDropEntryV2(10U, 1001U, 1, 0, 0)
                });

                Assert.IsFalse(renderer.ApplyCapturedLootResolved());
                Assert.AreEqual(1, renderer.MarkerCount);

                PlayerNetworkSessionController controller =
                    root.AddComponent<PlayerNetworkSessionController>();
                renderer.NetworkSessionController = controller;

                Assert.IsFalse(renderer.ApplyCapturedLootResolved());
                Assert.AreEqual(1, renderer.MarkerCount);
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        [Test]
        public async Task UpdateRendersCapturedDropSnapshotFromController()
        {
            GameObject root = new GameObject("DropRoot");
            try
            {
                PlayerNetworkSessionController controller =
                    root.AddComponent<PlayerNetworkSessionController>();
                PlayerDropMarkerRenderer renderer =
                    root.AddComponent<PlayerDropMarkerRenderer>();
                renderer.NetworkSessionController = controller;

                FakeConnector connector = new FakeConnector
                {
                    SessionIdToReturn = 22UL
                };
                connector.ReceivePacketsToReturn.Enqueue(JoinRoomResponsePacket(17U, 2));
                connector.ReceivePacketsToReturn.Enqueue(RoomListSnapshotPacket(
                    new PlayerRoomListEntry(17U, 2, 10)));
                connector.ReceivePacketsToReturn.Enqueue(DefaultJoinedRoomDetailPacket());
                connector.ReceivePacketsToReturn.Enqueue(ReadyRoomResponsePacket(17U, 2, 2));
                connector.ReceivePacketsToReturn.Enqueue(BattleStartPacket(17U, 11UL, 22UL));
                connector.ReceivePacketsToReturn.Enqueue(MonsterSpawnPacket(17U, 1001U, 2001U, 100));
                connector.ReceivePacketsToReturn.Enqueue(MonsterDeathPacket(17U, 1001U));
                connector.ReceivePacketsToReturn.Enqueue(DropListSnapshotV2Packet(
                    17U,
                    12345U,
                    new PlayerDropEntryV2(1U, 1001U, 1, -2500, 3000)));
                PlayerNetworkSession session =
                    new PlayerNetworkSession(connector, new FakeRudpSender());
                await session.ConnectAsync(PlayerServerEndpoint.Default);
                Assert.AreEqual(PlayerNetworkSessionStatus.Connected, session.Status);
                Assert.IsTrue(await session.RequestJoinRoomAsync(17U));
                Assert.IsTrue(await session.RequestReadyRoomAsync());
                Assert.IsTrue(await session.CaptureBattleStartAsync());
                Assert.IsTrue(await session.CaptureMonsterSpawnAsync());
                Assert.IsTrue(await session.CaptureMonsterDeathAsync());
                Assert.IsTrue(await session.CaptureDropListSnapshotV2Async());
                Assert.IsTrue(session.DropListSnapshotV2Captured);
                Assert.AreEqual(1, session.DropListSnapshotV2DropCount);
                InjectSession(controller, session);

                MethodInfo updateMethod = typeof(PlayerDropMarkerRenderer).GetMethod(
                    "Update",
                    BindingFlags.Instance | BindingFlags.NonPublic);
                Assert.NotNull(updateMethod);
                updateMethod.Invoke(renderer, null);

                Assert.AreEqual(1, renderer.MarkerCount);
                Assert.NotNull(renderer.GetMarker(1U));
            }
            finally
            {
                Object.DestroyImmediate(root);
            }
        }

        private static void InjectSession(
            PlayerNetworkSessionController controller,
            PlayerNetworkSession session)
        {
            FieldInfo field = typeof(PlayerNetworkSessionController).GetField(
                "session",
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.NotNull(field);
            field.SetValue(controller, session);
        }

        private static void AssertChildRendererUsesMaterial(
            GameObject target,
            string childName,
            Material expectedMaterial)
        {
            Transform child = target.transform.Find(childName);
            Assert.NotNull(child);
            Renderer renderer = child.GetComponent<Renderer>();
            Assert.NotNull(renderer);
            Assert.AreSame(expectedMaterial, renderer.sharedMaterial);
        }

        private static void AssertIdentityMarkerColor(
            GameObject target,
            string markerName,
            Color expectedColor)
        {
            Transform marker = target.transform.Find(markerName);
            Assert.NotNull(marker);
            Renderer renderer = marker.GetComponent<Renderer>();
            Assert.NotNull(renderer);
            Assert.NotNull(renderer.sharedMaterial);
            Color actualColor = ReadMaterialColor(renderer.sharedMaterial);
            Assert.That(actualColor.r, Is.EqualTo(expectedColor.r).Within(0.001f));
            Assert.That(actualColor.g, Is.EqualTo(expectedColor.g).Within(0.001f));
            Assert.That(actualColor.b, Is.EqualTo(expectedColor.b).Within(0.001f));
            Assert.That(actualColor.a, Is.EqualTo(expectedColor.a).Within(0.001f));
        }

        private static void AssertIdentityMarkerScale(
            GameObject target,
            string markerName,
            float expectedScale)
        {
            Transform marker = target.transform.Find(markerName);
            Assert.NotNull(marker);
            Assert.That(marker.localScale.x, Is.EqualTo(expectedScale).Within(0.001f));
            Assert.That(marker.localScale.y, Is.EqualTo(0.02f).Within(0.001f));
            Assert.That(marker.localScale.z, Is.EqualTo(expectedScale).Within(0.001f));
        }

        private static void AssertIdentityMarkerWorldPose(
            GameObject target,
            string markerName,
            float expectedWorldDiameter)
        {
            Transform marker = target.transform.Find(markerName);
            Assert.NotNull(marker);
            Assert.That(
                marker.lossyScale.x,
                Is.EqualTo(expectedWorldDiameter).Within(0.001f));
            Assert.That(
                marker.lossyScale.z,
                Is.EqualTo(expectedWorldDiameter).Within(0.001f));
            Assert.That(marker.position.y, Is.GreaterThan(0.04f));
        }

        private static Color ReadMaterialColor(Material material)
        {
            if (material.HasProperty("_BaseColor"))
            {
                return material.GetColor("_BaseColor");
            }

            return material.HasProperty("_Color") ?
                material.GetColor("_Color") :
                material.color;
        }

        private static Material CreateTestMaterial(string materialName, Color color)
        {
            Shader shader =
                Shader.Find("Standard") ??
                Shader.Find("Universal Render Pipeline/Lit") ??
                Shader.Find("Universal Render Pipeline/Unlit");
            Material material = new Material(shader);
            material.name = materialName;
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

        private sealed class FakeConnector : IPlayerNetworkConnector
        {
            public int SendPacketCallCount;
            public int ReceivePacketCallCount;
            public ulong SessionIdToReturn = 1UL;
            public Queue<byte[]> ReceivePacketsToReturn = new Queue<byte[]>();

            public bool HasPendingPacket => ReceivePacketsToReturn.Count > 0;

            public Task<PlayerNetworkConnectResult> ConnectAsync(
                PlayerServerEndpoint endpoint,
                string gameSessionToken,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                PlayerClientListSnapshot snapshot =
                    new PlayerClientListSnapshot(new[] { SessionIdToReturn });
                return Task.FromResult(
                    new PlayerNetworkConnectResult(SessionIdToReturn, snapshot));
            }

            public Task SendPacketAsync(byte[] packet, CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                ++SendPacketCallCount;
                return Task.CompletedTask;
            }

            public Task<byte[]> ReceivePacketAsync(CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                ++ReceivePacketCallCount;
                if (ReceivePacketsToReturn.Count == 0)
                {
                    throw new InvalidOperationException("no packet queued");
                }

                return Task.FromResult(ReceivePacketsToReturn.Dequeue());
            }

            public void Disconnect()
            {
            }

            public void Dispose()
            {
            }
        }

        private sealed class FakeRudpSender : IPlayerRudpSender
        {
            public Task<PlayerRudpHelloSendResult> SendHelloAsync(
                PlayerServerEndpoint endpoint,
                PlayerRudpHello hello,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                return Task.FromResult(
                    new PlayerRudpHelloSendResult(hello.SessionId, 1U, "127.0.0.1:50000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendAttackAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                uint targetHintMonsterId,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                return Task.FromResult(new PlayerRudpInputCommandSendResult(
                    sessionId,
                    1U,
                    1U,
                    targetHintMonsterId,
                    "127.0.0.1:50000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendMoveAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                short dirX,
                short dirY,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                return Task.FromResult(new PlayerRudpInputCommandSendResult(
                    sessionId,
                    1U,
                    1U,
                    0U,
                    "127.0.0.1:50000"));
            }

            public Task<PlayerRudpInputCommandSendResult> SendSpaceLootAsync(
                PlayerServerEndpoint endpoint,
                ulong sessionId,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                return Task.FromResult(new PlayerRudpInputCommandSendResult(
                    sessionId,
                    1U,
                    1U,
                    0U,
                    "127.0.0.1:50000"));
            }

            public bool TryReceiveStateSnapshot(out PlayerRudpStateSnapshot snapshot)
            {
                snapshot = default;
                return false;
            }

            public void Disconnect()
            {
            }

            public void Dispose()
            {
            }
        }

        private sealed class FakeVisualProvider : IRelease0VisualProvider
        {
            public readonly Material DropMaterial =
                CreateTestMaterial("FakeDropMaterial", Color.white);

            public int DropCallCount;

            public GameObject CreateArenaFloor() => new GameObject("FakeArenaFloor");
            public GameObject CreateLocalPlayer() => new GameObject("FakeLocalPlayer");
            public GameObject CreateRemotePlayer() => new GameObject("FakeRemotePlayer");
            public GameObject CreateMonster() => new GameObject("FakeMonster");

            public GameObject CreateDrop()
            {
                ++DropCallCount;
                GameObject root = new GameObject("FakeDrop");
                GameObject child = GameObject.CreatePrimitive(PrimitiveType.Sphere);
                child.name = "FakeDropModel";
                child.transform.SetParent(root.transform, false);
                child.GetComponent<Renderer>().sharedMaterial = DropMaterial;
                return root;
            }
        }

        private static byte[] JoinRoomResponsePacket(uint roomId, ushort playerCount)
        {
            byte[] packet = new byte[10];
            packet[1] = 0x0A;
            packet[2] = 0x01;
            packet[3] = 0x04;
            WriteU32BE(roomId, packet, 4);
            packet[8] = (byte)(playerCount >> 8);
            packet[9] = (byte)playerCount;
            return packet;
        }

        private static byte[] ReadyRoomResponsePacket(
            uint roomId,
            ushort readyPlayerCount,
            ushort totalPlayerCount)
        {
            byte[] packet = new byte[12];
            packet[1] = 0x0C;
            packet[2] = 0x01;
            packet[3] = 0x09;
            WriteU32BE(roomId, packet, 4);
            packet[8] = (byte)(readyPlayerCount >> 8);
            packet[9] = (byte)readyPlayerCount;
            packet[10] = (byte)(totalPlayerCount >> 8);
            packet[11] = (byte)totalPlayerCount;
            return packet;
        }

        private static byte[] BattleStartPacket(
            uint roomId,
            ulong playerASessionId,
            ulong playerBSessionId)
        {
            byte[] packet = new byte[24];
            packet[1] = 0x18;
            packet[2] = 0x01;
            packet[3] = 0x0A;
            WriteU32BE(roomId, packet, 4);
            WriteU64BE(playerASessionId, packet, 8);
            WriteU64BE(playerBSessionId, packet, 16);
            return packet;
        }

        private static byte[] MonsterSpawnPacket(
            uint roomId,
            uint monsterId,
            uint monsterTypeId,
            ushort maxHp)
        {
            byte[] packet = new byte[18];
            packet[1] = 0x12;
            packet[2] = 0x01;
            packet[3] = 0x0B;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(monsterId, packet, 8);
            WriteU32BE(monsterTypeId, packet, 12);
            packet[16] = (byte)(maxHp >> 8);
            packet[17] = (byte)maxHp;
            return packet;
        }

        private static byte[] MonsterDeathPacket(uint roomId, uint monsterId)
        {
            byte[] packet = new byte[12];
            packet[1] = 0x0C;
            packet[2] = 0x01;
            packet[3] = 0x0D;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(monsterId, packet, 8);
            return packet;
        }

        private static byte[] DropListSnapshotV2Packet(
            uint roomId,
            uint scatterSeed,
            params PlayerDropEntryV2[] drops)
        {
            byte[] packet = new byte[14 + (drops.Length * 18)];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x1A;
            WriteU32BE(roomId, packet, 4);
            WriteU32BE(scatterSeed, packet, 8);
            packet[12] = (byte)(drops.Length >> 8);
            packet[13] = (byte)drops.Length;

            int offset = 14;
            foreach (PlayerDropEntryV2 drop in drops)
            {
                WriteU32BE(drop.DropId, packet, offset);
                WriteU32BE(drop.ItemId, packet, offset + 4);
                packet[offset + 8] = (byte)(drop.Quantity >> 8);
                packet[offset + 9] = (byte)drop.Quantity;
                WriteI32BE(drop.PosX, packet, offset + 10);
                WriteI32BE(drop.PosY, packet, offset + 14);
                offset += 18;
            }

            return packet;
        }

        private static byte[] RoomListSnapshotPacket(params PlayerRoomListEntry[] rooms)
        {
            int packetSize = 6;
            foreach (PlayerRoomListEntry room in rooms)
            {
                packetSize += 10 + System.Text.Encoding.UTF8.GetByteCount(room.Title ?? string.Empty);
            }

            byte[] packet = new byte[packetSize];
            packet[0] = (byte)(packet.Length >> 8);
            packet[1] = (byte)packet.Length;
            packet[2] = 0x01;
            packet[3] = 0x07;
            packet[4] = (byte)(rooms.Length >> 8);
            packet[5] = (byte)rooms.Length;

            int offset = 6;
            foreach (PlayerRoomListEntry room in rooms)
            {
                WriteU32BE(room.RoomId, packet, offset);
                packet[offset + 4] = (byte)(room.PlayerCount >> 8);
                packet[offset + 5] = (byte)room.PlayerCount;
                packet[offset + 6] = (byte)(room.MaxPlayers >> 8);
                packet[offset + 7] = (byte)room.MaxPlayers;
                packet[offset + 8] = (byte)room.Status;
                byte[] titleBytes = System.Text.Encoding.UTF8.GetBytes(room.Title ?? string.Empty);
                packet[offset + 9] = (byte)titleBytes.Length;
                System.Buffer.BlockCopy(titleBytes, 0, packet, offset + 10, titleBytes.Length);
                offset += 10 + titleBytes.Length;
            }

            return packet;
        }

        private static byte[] DefaultJoinedRoomDetailPacket()
        {
            return RoomDetailStatePacket(
                17U,
                PlayerRoomStatus.Open,
                "Room17",
                10,
                PlayerTcpPacket.RoomActionLeaveRoom | PlayerTcpPacket.RoomActionReady,
                new[]
                {
                    new PlayerRoomMemberEntry(11UL, "Player11", false),
                    new PlayerRoomMemberEntry(22UL, "Player22", false)
                });
        }

        private static byte[] RoomDetailStatePacket(
            uint roomId,
            PlayerRoomStatus status,
            string title,
            byte maxPlayers,
            ushort selfActionMask,
            PlayerRoomMemberEntry[] members)
        {
            int size = 4 + 4 + 1 + 1 + title.Length + 1 + 1 + 2 + 1;
            foreach (PlayerRoomMemberEntry member in members)
            {
                size += 4 + 1 + member.Nickname.Length + 1;
            }

            byte[] packet = new byte[size];
            packet[0] = (byte)(size >> 8);
            packet[1] = (byte)size;
            packet[2] = 0x01;
            packet[3] = 0x1D;
            int offset = 4;
            WriteU32BE(roomId, packet, offset);
            offset += 4;
            packet[offset++] = (byte)status;
            packet[offset++] = (byte)title.Length;
            for (int index = 0; index < title.Length; ++index)
            {
                packet[offset++] = (byte)title[index];
            }
            packet[offset++] = maxPlayers;
            packet[offset++] = (byte)members.Length;
            foreach (PlayerRoomMemberEntry member in members)
            {
                WriteU32BE((uint)member.SessionId, packet, offset);
                offset += 4;
                packet[offset++] = (byte)member.Nickname.Length;
                for (int index = 0; index < member.Nickname.Length; ++index)
                {
                    packet[offset++] = (byte)member.Nickname[index];
                }
                packet[offset++] = member.Ready ? (byte)1 : (byte)0;
            }
            packet[offset++] = (byte)(selfActionMask >> 8);
            packet[offset++] = (byte)selfActionMask;
            packet[offset++] = 0;

            return packet;
        }

        private static void WriteU32BE(uint value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 24);
            packet[offset + 1] = (byte)(value >> 16);
            packet[offset + 2] = (byte)(value >> 8);
            packet[offset + 3] = (byte)value;
        }

        private static void WriteU64BE(ulong value, byte[] packet, int offset)
        {
            packet[offset] = (byte)(value >> 56);
            packet[offset + 1] = (byte)(value >> 48);
            packet[offset + 2] = (byte)(value >> 40);
            packet[offset + 3] = (byte)(value >> 32);
            packet[offset + 4] = (byte)(value >> 24);
            packet[offset + 5] = (byte)(value >> 16);
            packet[offset + 6] = (byte)(value >> 8);
            packet[offset + 7] = (byte)value;
        }

        private static void WriteI32BE(int value, byte[] packet, int offset)
        {
            WriteU32BE(unchecked((uint)value), packet, offset);
        }
    }
}
