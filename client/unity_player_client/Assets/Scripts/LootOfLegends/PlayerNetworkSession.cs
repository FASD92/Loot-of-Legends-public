using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerNetworkSession : IDisposable
    {
        private const int RoomListCaptureTimeoutMilliseconds = 250;
        private const int LootCaptureTimeoutMilliseconds = 3000;
        public const string SessionReplacedMessage =
            "다른 클라이언트에서 접속되어 연결이 종료되었습니다";

        private readonly object syncRoot = new object();
        private readonly SemaphoreSlim tcpReceiveSemaphore = new SemaphoreSlim(1, 1);
        private readonly SemaphoreSlim tcpSendSemaphore = new SemaphoreSlim(1, 1);
        private readonly IPlayerNetworkConnector connector;
        private readonly IPlayerRudpSender rudpSender;
        private readonly Queue<byte[]> deferredTcpPackets = new Queue<byte[]>();
        private CancellationTokenSource connectCancellation;
        private PlayerServerEndpoint endpoint;
        private PlayerNetworkSessionStatus status;
        private ulong sessionId;
        private PlayerClientListSnapshot clientListSnapshot;
        private uint currentRoomId;
        private ushort currentRoomPlayerCount;
        private ushort currentRoomReadyPlayerCount;
        private bool roomVisibilityCommandInFlight;
        private bool roomDetailCaptured;
        private PlayerRoomDetailState roomDetailState;
        private bool lobbyReturnCaptured;
        private uint lobbyReturnPreviousRoomId;
        private PlayerLobbyReturnReason lobbyReturnReason;
        private bool battleStarted;
        private uint battleStartRoomId;
        private ulong battleStartPlayerASessionId;
        private ulong battleStartPlayerBSessionId;
        private bool battleLoadEntryCaptured;
        private uint battleLoadEntryRoomId;
        private ulong battleInstanceId;
        private ulong[] battleLoadPlayerSessionIds = Array.Empty<ulong>();
        private bool arenaLoadCompleteSent;
        private bool arenaGameplayStarted;
        private uint arenaGameplayStartRoomId;
        private ulong arenaGameplayStartBattleInstanceId;
        private bool monsterSpawned;
        private uint monsterSpawnRoomId;
        private uint monsterId;
        private uint monsterTypeId;
        private ushort monsterMaxHp;
        private bool monsterHealthSnapshotCaptured;
        private uint monsterHealthRoomId;
        private uint monsterHealthMonsterId;
        private ushort monsterCurrentHp;
        private ushort monsterHealthMaxHp;
        private bool monsterDeathCaptured;
        private uint monsterDeathRoomId;
        private uint monsterDeathMonsterId;
        private bool dropListSnapshotV2Captured;
        private PlayerDropListSnapshotV2 dropListSnapshotV2;
        private PlayerRoomListSnapshot roomListSnapshot;
        private bool rudpHelloSent;
        private ulong rudpHelloSessionId;
        private uint rudpHelloSequence;
        private string rudpHelloLocalEndpoint = string.Empty;
        private bool rudpAttackSent;
        private ulong rudpAttackSessionId;
        private uint rudpAttackSequence;
        private uint rudpAttackCommandSequence;
        private uint rudpAttackTargetHintMonsterId;
        private string rudpAttackLocalEndpoint = string.Empty;
        private bool rudpSpaceLootSent;
        private ulong rudpSpaceLootSessionId;
        private uint rudpSpaceLootSequence;
        private uint rudpSpaceLootCommandSequence;
        private string rudpSpaceLootLocalEndpoint = string.Empty;
        private bool rudpMoveSent;
        private ulong rudpMoveSessionId;
        private uint rudpMoveSequence;
        private uint rudpMoveCommandSequence;
        private string rudpMoveLocalEndpoint = string.Empty;
        private bool stateSnapshotCaptured;
        private PlayerRudpStateSnapshot stateSnapshot;
        private bool lootResolvedCaptured;
        private PlayerLootResolved lootResolved;
        private bool lootRejectedCaptured;
        private PlayerLootRejected lootRejected;
        private bool inventorySnapshotCaptured;
        private PlayerInventorySnapshot inventorySnapshot;
        private bool battleFinalRankingCaptured;
        private PlayerBattleFinalRanking battleFinalRanking;
        private string lastError = string.Empty;

        public PlayerNetworkSession(IPlayerNetworkConnector connector)
            : this(connector, new PlayerUdpRudpSender())
        {
        }

        public PlayerNetworkSession(IPlayerNetworkConnector connector, IPlayerRudpSender rudpSender)
        {
            this.connector = connector ?? throw new ArgumentNullException(nameof(connector));
            this.rudpSender = rudpSender ?? throw new ArgumentNullException(nameof(rudpSender));
            endpoint = PlayerServerEndpoint.Default;
            status = PlayerNetworkSessionStatus.Disconnected;
        }

        public PlayerServerEndpoint Endpoint
        {
            get
            {
                lock (syncRoot)
                {
                    return endpoint;
                }
            }
        }

        public PlayerNetworkSessionStatus Status
        {
            get
            {
                lock (syncRoot)
                {
                    return status;
                }
            }
        }

        public string LastError
        {
            get
            {
                lock (syncRoot)
                {
                    return lastError;
                }
            }
        }

        public ulong SessionId
        {
            get
            {
                lock (syncRoot)
                {
                    return sessionId;
                }
            }
        }

        public int ClientSessionCount
        {
            get
            {
                lock (syncRoot)
                {
                    return clientListSnapshot.Count;
                }
            }
        }

        public bool SelfListedInClientListSnapshot
        {
            get
            {
                lock (syncRoot)
                {
                    return clientListSnapshot.ContainsSession(sessionId);
                }
            }
        }

        public ulong[] ClientSessionIds
        {
            get
            {
                lock (syncRoot)
                {
                    return clientListSnapshot.ToArray();
                }
            }
        }

        public uint CurrentRoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return currentRoomId;
                }
            }
        }

        public ushort CurrentRoomPlayerCount
        {
            get
            {
                lock (syncRoot)
                {
                    return currentRoomPlayerCount;
                }
            }
        }

        public ushort CurrentRoomReadyPlayerCount
        {
            get
            {
                lock (syncRoot)
                {
                    return currentRoomReadyPlayerCount;
                }
            }
        }

        public byte CurrentRoomMaxPlayers
        {
            get
            {
                lock (syncRoot)
                {
                    return roomDetailCaptured ? roomDetailState.MaxPlayers : (byte)0;
                }
            }
        }

        public bool BattleStarted
        {
            get
            {
                lock (syncRoot)
                {
                    return battleStarted;
                }
            }
        }

        public uint BattleStartRoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return battleStartRoomId;
                }
            }
        }

        public ulong BattleStartPlayerASessionId
        {
            get
            {
                lock (syncRoot)
                {
                    return battleStartPlayerASessionId;
                }
            }
        }

        public ulong BattleStartPlayerBSessionId
        {
            get
            {
                lock (syncRoot)
                {
                    return battleStartPlayerBSessionId;
                }
            }
        }

        public bool BattleLoadEntryCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return battleLoadEntryCaptured;
                }
            }
        }

        public uint BattleLoadEntryRoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return battleLoadEntryRoomId;
                }
            }
        }

        public ulong BattleInstanceId
        {
            get
            {
                lock (syncRoot)
                {
                    return battleInstanceId;
                }
            }
        }

        public ulong[] BattleLoadPlayerSessionIds
        {
            get
            {
                lock (syncRoot)
                {
                    return CopyUlongs(battleLoadPlayerSessionIds);
                }
            }
        }

        public bool ArenaLoadCompleteSent
        {
            get
            {
                lock (syncRoot)
                {
                    return arenaLoadCompleteSent;
                }
            }
        }

        public bool ArenaGameplayStarted
        {
            get
            {
                lock (syncRoot)
                {
                    return arenaGameplayStarted;
                }
            }
        }

        public uint ArenaGameplayStartRoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return arenaGameplayStartRoomId;
                }
            }
        }

        public ulong ArenaGameplayStartBattleInstanceId
        {
            get
            {
                lock (syncRoot)
                {
                    return arenaGameplayStartBattleInstanceId;
                }
            }
        }

        public bool MonsterSpawned
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterSpawned;
                }
            }
        }

        public uint MonsterSpawnRoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterSpawnRoomId;
                }
            }
        }

        public uint MonsterId
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterId;
                }
            }
        }

        public uint MonsterTypeId
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterTypeId;
                }
            }
        }

        public ushort MonsterMaxHp
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterMaxHp;
                }
            }
        }

        public bool MonsterHealthSnapshotCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterHealthSnapshotCaptured;
                }
            }
        }

        public uint MonsterHealthRoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterHealthRoomId;
                }
            }
        }

        public uint MonsterHealthMonsterId
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterHealthMonsterId;
                }
            }
        }

        public ushort MonsterCurrentHp
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterCurrentHp;
                }
            }
        }

        public ushort MonsterHealthMaxHp
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterHealthMaxHp;
                }
            }
        }

        public bool MonsterDeathCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterDeathCaptured;
                }
            }
        }

        public uint MonsterDeathRoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterDeathRoomId;
                }
            }
        }

        public uint MonsterDeathMonsterId
        {
            get
            {
                lock (syncRoot)
                {
                    return monsterDeathMonsterId;
                }
            }
        }

        public bool DropListSnapshotV2Captured
        {
            get
            {
                lock (syncRoot)
                {
                    return dropListSnapshotV2Captured;
                }
            }
        }

        public uint DropListSnapshotV2RoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return dropListSnapshotV2Captured ? dropListSnapshotV2.RoomId : 0U;
                }
            }
        }

        public uint DropListSnapshotV2ScatterSeed
        {
            get
            {
                lock (syncRoot)
                {
                    return dropListSnapshotV2Captured ? dropListSnapshotV2.ScatterSeed : 0U;
                }
            }
        }

        public int DropListSnapshotV2DropCount
        {
            get
            {
                lock (syncRoot)
                {
                    return dropListSnapshotV2Captured ? dropListSnapshotV2.Count : 0;
                }
            }
        }

        public PlayerDropEntryV2[] DropListSnapshotV2Drops
        {
            get
            {
                lock (syncRoot)
                {
                    return dropListSnapshotV2Captured ?
                        dropListSnapshotV2.ToArray() :
                        new PlayerDropEntryV2[0];
                }
            }
        }

        public int RoomListCount
        {
            get
            {
                lock (syncRoot)
                {
                    return roomListSnapshot.Count;
                }
            }
        }

        public PlayerRoomListEntry[] RoomListEntries
        {
            get
            {
                lock (syncRoot)
                {
                    return roomListSnapshot.ToArray();
                }
            }
        }

        public bool RoomDetailCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return roomDetailCaptured;
                }
            }
        }

        public PlayerRoomStatus CurrentRoomStatus
        {
            get
            {
                lock (syncRoot)
                {
                    return roomDetailCaptured ? roomDetailState.Status : PlayerRoomStatus.Open;
                }
            }
        }

        public string RoomTitle
        {
            get
            {
                lock (syncRoot)
                {
                    return roomDetailCaptured ? roomDetailState.Title : string.Empty;
                }
            }
        }

        public PlayerRoomMemberEntry[] RoomMembers
        {
            get
            {
                lock (syncRoot)
                {
                    return roomDetailCaptured ?
                        roomDetailState.MembersToArray() :
                        new PlayerRoomMemberEntry[0];
                }
            }
        }

        public ushort SelfRoomActionMask
        {
            get
            {
                lock (syncRoot)
                {
                    return roomDetailCaptured ? roomDetailState.SelfActionMask : (ushort)0;
                }
            }
        }

        public PlayerRoomTargetActionEntry[] RoomTargetActions
        {
            get
            {
                lock (syncRoot)
                {
                    return roomDetailCaptured ?
                        roomDetailState.TargetActionsToArray() :
                        new PlayerRoomTargetActionEntry[0];
                }
            }
        }

        public bool LobbyReturnCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return lobbyReturnCaptured;
                }
            }
        }

        public uint LobbyReturnPreviousRoomId
        {
            get
            {
                lock (syncRoot)
                {
                    return lobbyReturnPreviousRoomId;
                }
            }
        }

        public PlayerLobbyReturnReason LobbyReturnReason
        {
            get
            {
                lock (syncRoot)
                {
                    return lobbyReturnReason;
                }
            }
        }

        public bool RudpHelloSent
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpHelloSent;
                }
            }
        }

        public ulong RudpHelloSessionId
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpHelloSessionId;
                }
            }
        }

        public uint RudpHelloSequence
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpHelloSequence;
                }
            }
        }

        public string RudpHelloLocalEndpoint
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpHelloLocalEndpoint;
                }
            }
        }

        public bool RudpAttackSent
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpAttackSent;
                }
            }
        }

        public ulong RudpAttackSessionId
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpAttackSessionId;
                }
            }
        }

        public uint RudpAttackSequence
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpAttackSequence;
                }
            }
        }

        public uint RudpAttackCommandSequence
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpAttackCommandSequence;
                }
            }
        }

        public uint RudpAttackTargetHintMonsterId
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpAttackTargetHintMonsterId;
                }
            }
        }

        public string RudpAttackLocalEndpoint
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpAttackLocalEndpoint;
                }
            }
        }

        public bool RudpSpaceLootSent
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpSpaceLootSent;
                }
            }
        }

        public ulong RudpSpaceLootSessionId
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpSpaceLootSessionId;
                }
            }
        }

        public uint RudpSpaceLootSequence
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpSpaceLootSequence;
                }
            }
        }

        public uint RudpSpaceLootCommandSequence
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpSpaceLootCommandSequence;
                }
            }
        }

        public string RudpSpaceLootLocalEndpoint
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpSpaceLootLocalEndpoint;
                }
            }
        }

        public bool RudpMoveSent
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpMoveSent;
                }
            }
        }

        public ulong RudpMoveSessionId
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpMoveSessionId;
                }
            }
        }

        public uint RudpMoveSequence
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpMoveSequence;
                }
            }
        }

        public uint RudpMoveCommandSequence
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpMoveCommandSequence;
                }
            }
        }

        public string RudpMoveLocalEndpoint
        {
            get
            {
                lock (syncRoot)
                {
                    return rudpMoveLocalEndpoint;
                }
            }
        }

        public bool StateSnapshotCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return stateSnapshotCaptured;
                }
            }
        }

        public PlayerRudpStateSnapshot StateSnapshot
        {
            get
            {
                lock (syncRoot)
                {
                    return stateSnapshot;
                }
            }
        }

        public bool LootResolvedCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return lootResolvedCaptured;
                }
            }
        }

        public PlayerLootResolved LootResolved
        {
            get
            {
                lock (syncRoot)
                {
                    return lootResolved;
                }
            }
        }

        public bool LootRejectedCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return lootRejectedCaptured;
                }
            }
        }

        public PlayerLootRejected LootRejected
        {
            get
            {
                lock (syncRoot)
                {
                    return lootRejected;
                }
            }
        }

        public bool InventorySnapshotCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return inventorySnapshotCaptured;
                }
            }
        }

        public PlayerInventorySnapshot InventorySnapshot
        {
            get
            {
                lock (syncRoot)
                {
                    return inventorySnapshot;
                }
            }
        }

        public PlayerInventoryEntry[] InventorySnapshotEntries
        {
            get
            {
                lock (syncRoot)
                {
                    return inventorySnapshotCaptured ?
                        inventorySnapshot.ToArray() :
                        new PlayerInventoryEntry[0];
                }
            }
        }

        public bool BattleFinalRankingCaptured
        {
            get
            {
                lock (syncRoot)
                {
                    return battleFinalRankingCaptured;
                }
            }
        }

        public PlayerBattleFinalRanking BattleFinalRanking
        {
            get
            {
                lock (syncRoot)
                {
                    return battleFinalRanking;
                }
            }
        }

        public async Task ConnectAsync(PlayerServerEndpoint nextEndpoint)
        {
            string gameSessionToken = string.Empty;
            GameSessionRoot root = GameSessionRoot.Instance;
            if (root != null && root.HasAdmission)
            {
                gameSessionToken = root.GameSessionToken;
            }

            await ConnectAsync(nextEndpoint, gameSessionToken);
        }

        public async Task ConnectAsync(
            PlayerServerEndpoint nextEndpoint,
            string gameSessionToken)
        {
            DisconnectBeforeReconnectIfNeeded();

            if (!nextEndpoint.IsValid)
            {
                lock (syncRoot)
                {
                    endpoint = nextEndpoint;
                    status = PlayerNetworkSessionStatus.Failed;
                    sessionId = 0UL;
                    ClearDeferredTcpPacketState();
                    ClearClientListSnapshotState();
                    ClearRudpHelloState();
                    ClearRoomState();
                    lastError = "invalid endpoint";
                }
                return;
            }

            CancellationTokenSource cancellation = new CancellationTokenSource();
            lock (syncRoot)
            {
                connectCancellation = cancellation;
                endpoint = nextEndpoint;
                status = PlayerNetworkSessionStatus.Connecting;
                sessionId = 0UL;
                ClearDeferredTcpPacketState();
                ClearClientListSnapshotState();
                ClearRudpHelloState();
                ClearRoomState();
                lastError = string.Empty;
            }

            try
            {
                PlayerNetworkConnectResult result =
                    await connector.ConnectAsync(
                        nextEndpoint,
                        gameSessionToken ?? string.Empty,
                        cancellation.Token);
                if (!result.IsValid)
                {
                    throw new InvalidOperationException("invalid connect result");
                }

                lock (syncRoot)
                {
                    if (connectCancellation != cancellation || cancellation.IsCancellationRequested)
                    {
                        return;
                    }
                }

                PlayerRudpHello hello = PlayerRudpHello.ForSession(result.SessionId);
                PlayerRudpHelloSendResult helloResult =
                    await rudpSender.SendHelloAsync(nextEndpoint, hello, cancellation.Token);
                if (!helloResult.IsSent)
                {
                    throw new InvalidOperationException("invalid RUDP Hello result");
                }

                lock (syncRoot)
                {
                    if (connectCancellation != cancellation || cancellation.IsCancellationRequested)
                    {
                        return;
                    }

                    status = PlayerNetworkSessionStatus.Connected;
                    sessionId = result.SessionId;
                    clientListSnapshot = result.ClientListSnapshot;
                    roomListSnapshot = result.InitialRoomListSnapshot;
                    rudpHelloSent = true;
                    rudpHelloSessionId = helloResult.SessionId;
                    rudpHelloSequence = helloResult.Sequence;
                    rudpHelloLocalEndpoint = helloResult.LocalEndpoint;
                    lastError = string.Empty;
                }
            }
            catch (OperationCanceledException)
            {
                lock (syncRoot)
                {
                    if (connectCancellation == cancellation)
                    {
                        status = PlayerNetworkSessionStatus.Disconnected;
                        sessionId = 0UL;
                        ClearDeferredTcpPacketState();
                        ClearClientListSnapshotState();
                        ClearRudpHelloState();
                        ClearRoomState();
                        lastError = string.Empty;
                    }
                }
            }
            catch (Exception ex)
            {
                bool shouldDisconnectConnector = false;
                bool shouldDisconnectRudpSender = false;
                lock (syncRoot)
                {
                    if (connectCancellation == cancellation)
                    {
                        status = PlayerNetworkSessionStatus.Failed;
                        sessionId = 0UL;
                        ClearDeferredTcpPacketState();
                        ClearClientListSnapshotState();
                        ClearRudpHelloState();
                        ClearRoomState();
                        lastError = ex.Message;
                        shouldDisconnectConnector = true;
                        shouldDisconnectRudpSender = true;
                    }
                }

                if (shouldDisconnectConnector)
                {
                    connector.Disconnect();
                }
                if (shouldDisconnectRudpSender)
                {
                    rudpSender.Disconnect();
                }
            }
            finally
            {
                lock (syncRoot)
                {
                    if (connectCancellation == cancellation)
                    {
                        connectCancellation = null;
                    }
                }
                cancellation.Dispose();
            }
        }

        public async Task<bool> CaptureRoomListSnapshotAsync()
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
            }

            using (CancellationTokenSource cancellation =
                new CancellationTokenSource(RoomListCaptureTimeoutMilliseconds))
            {
                try
                {
                    await ReceiveRoomListSnapshotAsync(cancellation.Token);
                    lock (syncRoot)
                    {
                        if (status != PlayerNetworkSessionStatus.Connected)
                        {
                            return false;
                        }

                        lastError = string.Empty;
                    }

                    return true;
                }
                catch (OperationCanceledException)
                {
                    MarkRoomListCaptureUnavailable();
                    return false;
                }
                catch (Exception ex)
                {
                    MarkCommandSendFailed(ex);
                    return false;
                }
            }
        }

        public async Task<bool> DrainIncomingTcpStateAsync()
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
            }

            if (!connector.HasPendingPacket ||
                !await tcpReceiveSemaphore.WaitAsync(0))
            {
                return false;
            }

            try
            {
                if (!connector.HasPendingPacket)
                {
                    return false;
                }

                byte[] packet = await connector.ReceivePacketAsync(CancellationToken.None);
                if (TryApplyStateSnapshotPacket(packet))
                {
                    return true;
                }

                EnqueueDeferredTcpPacket(packet);
                return false;
            }
            catch (InvalidOperationException)
            {
                return false;
            }
            finally
            {
                tcpReceiveSemaphore.Release();
            }
        }

        public Task<bool> RequestCreateRoomAsync()
        {
            return RequestCreateRoomAsync("Room", PlayerTcpPacket.CreateRoomMaxCapacity);
        }

        public async Task<bool> RequestCreateRoomAsync(string roomTitle, int maxPlayers)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeCreateRoomRequest(roomTitle, maxPlayers),
                    CancellationToken.None);

                byte[] responsePacket =
                    await ReceiveNextCommandResponsePacketAsync(CancellationToken.None);
                if (PlayerTcpPacket.TryParseError(
                        responsePacket,
                        out ushort failedType,
                        out ushort errorCode) &&
                    failedType == PlayerTcpPacket.CreateRoomRequestPacketType)
                {
                    MarkRoomCommandRejected("create room", errorCode);
                    return false;
                }

                if (!PlayerTcpPacket.TryParseCreateRoomResponse(
                        responsePacket,
                        out uint roomId,
                        out ushort playerCount) ||
                    roomId == 0U ||
                    playerCount == 0)
                {
                    throw new InvalidOperationException("invalid create room response packet");
                }

                await ReceiveRoomVisibilityAfterCommandAsync(
                    roomId,
                    playerCount,
                    CancellationToken.None);

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    currentRoomId = roomId;
                    currentRoomPlayerCount = playerCount;
                    if (!roomDetailCaptured || roomDetailState.RoomId != roomId)
                    {
                        currentRoomReadyPlayerCount = 0;
                    }
                    ClearRudpMoveState();
                    ClearStateSnapshotState();
                    ClearBattleState();
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> RequestJoinRoomAsync(uint roomId)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (roomId == 0U)
                {
                    lastError = "invalid room id";
                    return false;
                }
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeJoinRoomRequest(roomId),
                    CancellationToken.None);

                byte[] responsePacket =
                    await ReceiveNextCommandResponsePacketAsync(CancellationToken.None);
                if (PlayerTcpPacket.TryParseError(
                        responsePacket,
                        out ushort failedType,
                        out ushort errorCode) &&
                    failedType == PlayerTcpPacket.JoinRoomRequestPacketType)
                {
                    MarkRoomCommandRejected("join room", errorCode);
                    return false;
                }

                if (!PlayerTcpPacket.TryParseJoinRoomResponse(
                        responsePacket,
                        out uint joinedRoomId,
                        out ushort playerCount) ||
                    joinedRoomId == 0U ||
                    joinedRoomId != roomId ||
                    playerCount == 0)
                {
                    throw new InvalidOperationException("invalid join room response packet");
                }

                await ReceiveRoomVisibilityAfterCommandAsync(
                    joinedRoomId,
                    playerCount,
                    CancellationToken.None);

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    currentRoomId = joinedRoomId;
                    currentRoomPlayerCount = playerCount;
                    if (!roomDetailCaptured || roomDetailState.RoomId != joinedRoomId)
                    {
                        currentRoomReadyPlayerCount = 0;
                    }
                    ClearRudpMoveState();
                    ClearStateSnapshotState();
                    ClearBattleState();
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> RequestReadyRoomAsync()
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }

                expectedRoomId = currentRoomId;
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeReadyRoomRequest(),
                    CancellationToken.None);

                byte[] responsePacket =
                    await ReceiveNextCommandResponsePacketAsync(CancellationToken.None);
                if (PlayerTcpPacket.TryParseError(
                        responsePacket,
                        out ushort failedType,
                        out ushort errorCode) &&
                    failedType == PlayerTcpPacket.ReadyRoomRequestPacketType)
                {
                    MarkRoomCommandRejected("ready room", errorCode);
                    return false;
                }

                if (!PlayerTcpPacket.TryParseReadyRoomResponse(
                        responsePacket,
                        out uint roomId,
                        out ushort readyPlayerCount,
                        out ushort totalPlayerCount) ||
                    roomId == 0U ||
                    roomId != expectedRoomId ||
                    readyPlayerCount == 0 ||
                    totalPlayerCount == 0 ||
                    readyPlayerCount > totalPlayerCount)
                {
                    throw new InvalidOperationException("invalid ready room response packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    currentRoomId = roomId;
                    currentRoomReadyPlayerCount = readyPlayerCount;
                    currentRoomPlayerCount = totalPlayerCount;
                    lastError = string.Empty;
                }

                await DrainPendingTcpStateSnapshotsAsync();

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> RequestUnreadyRoomAsync()
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }

                expectedRoomId = currentRoomId;
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeUnreadyRoomRequest(),
                    CancellationToken.None);

                byte[] responsePacket =
                    await ReceiveNextCommandResponsePacketAsync(CancellationToken.None);
                if (PlayerTcpPacket.TryParseError(
                        responsePacket,
                        out ushort failedType,
                        out ushort errorCode) &&
                    failedType == PlayerTcpPacket.UnreadyRoomRequestPacketType)
                {
                    MarkRoomCommandRejected("unready room", errorCode);
                    return false;
                }

                if (!PlayerTcpPacket.TryParseUnreadyRoomResponse(responsePacket, out uint roomId) ||
                    roomId != expectedRoomId)
                {
                    throw new InvalidOperationException("invalid unready room response packet");
                }

                await DrainPendingTcpStateSnapshotsAsync();
                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    currentRoomId = roomId;
                    currentRoomReadyPlayerCount = CountReadyMembers();
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> RequestHostStartBattleAsync()
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }

                expectedRoomId = currentRoomId;
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeHostStartBattleRequest(),
                    CancellationToken.None);

                byte[] responsePacket =
                    await ReceiveNextCommandResponsePacketAsync(CancellationToken.None);
                if (PlayerTcpPacket.TryParseError(
                        responsePacket,
                        out ushort failedType,
                        out ushort errorCode) &&
                    failedType == PlayerTcpPacket.HostStartBattleRequestPacketType)
                {
                    MarkRoomCommandRejected("host start battle", errorCode);
                    return false;
                }

                if (!PlayerTcpPacket.TryParseHostStartBattleResponse(
                        responsePacket,
                        out uint roomId) ||
                    roomId != expectedRoomId)
                {
                    throw new InvalidOperationException("invalid host start battle response packet");
                }

                await DrainPendingTcpStateSnapshotsAsync();
                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> RequestHostKickAsync(uint targetSessionId)
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (targetSessionId == 0U)
                {
                    lastError = "invalid target";
                    return false;
                }

                expectedRoomId = currentRoomId;
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeHostKickRequest(targetSessionId),
                    CancellationToken.None);

                byte[] responsePacket =
                    await ReceiveNextCommandResponsePacketAsync(CancellationToken.None);
                if (PlayerTcpPacket.TryParseError(
                        responsePacket,
                        out ushort failedType,
                        out ushort errorCode) &&
                    failedType == PlayerTcpPacket.HostKickRequestPacketType)
                {
                    MarkRoomCommandRejected("host kick", errorCode);
                    return false;
                }

                if (!PlayerTcpPacket.TryParseHostKickResponse(
                        responsePacket,
                        out uint roomId,
                        out uint kickedSessionId) ||
                    roomId != expectedRoomId ||
                    kickedSessionId != targetSessionId)
                {
                    throw new InvalidOperationException("invalid host kick response packet");
                }

                await DrainPendingTcpStateSnapshotsAsync();
                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> RequestLeaveRoomAsync()
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }

                expectedRoomId = currentRoomId;
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeLeaveRoomRequest(),
                    CancellationToken.None);

                byte[] responsePacket =
                    await ReceiveNextCommandResponsePacketAsync(CancellationToken.None);
                if (PlayerTcpPacket.TryParseError(
                        responsePacket,
                        out ushort failedType,
                        out ushort errorCode) &&
                    failedType == PlayerTcpPacket.LeaveRoomRequestPacketType)
                {
                    MarkRoomCommandRejected("leave room", errorCode);
                    return false;
                }

                if (!PlayerTcpPacket.TryParseLeaveRoomResponse(
                        responsePacket,
                        out uint roomId) ||
                    roomId != expectedRoomId)
                {
                    throw new InvalidOperationException("invalid leave room response packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    ClearRoomState();
                    lastError = string.Empty;
                }

                await DrainPendingTcpStateSnapshotsAsync();
                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureBattleStartAsync()
        {
            uint expectedRoomId;
            ulong expectedSessionId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }

                expectedRoomId = currentRoomId;
                expectedSessionId = sessionId;
                if (battleStarted &&
                    battleStartRoomId == expectedRoomId &&
                    battleStartPlayerASessionId != 0UL &&
                    battleStartPlayerBSessionId != 0UL &&
                    battleStartPlayerASessionId != battleStartPlayerBSessionId &&
                    (battleStartPlayerASessionId == expectedSessionId ||
                     battleStartPlayerBSessionId == expectedSessionId))
                {
                    lastError = string.Empty;
                    return true;
                }
            }

            try
            {
                byte[] packet = await ReceivePacketExclusiveAsync(CancellationToken.None);
                if (!PlayerTcpPacket.TryParseBattleStart(
                        packet,
                        out uint roomId,
                        out ulong playerASessionId,
                        out ulong playerBSessionId) ||
                    roomId == 0U ||
                    roomId != expectedRoomId ||
                    playerASessionId == 0UL ||
                    playerBSessionId == 0UL ||
                    playerASessionId == playerBSessionId ||
                    (playerASessionId != expectedSessionId && playerBSessionId != expectedSessionId))
                {
                    throw new InvalidOperationException("invalid battle start packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    battleStarted = true;
                    battleStartRoomId = roomId;
                    battleStartPlayerASessionId = playerASessionId;
                    battleStartPlayerBSessionId = playerBSessionId;
                    ClearBattleFinalRankingState();
                    ClearArenaLoadState();
                    ClearMonsterSpawnState();
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureBattleLoadEntryAsync()
        {
            uint expectedRoomId;
            ulong expectedSessionId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!battleStarted)
                {
                    lastError = "battle not started";
                    return false;
                }
                if (battleLoadEntryCaptured && !arenaGameplayStarted)
                {
                    lastError = "arena gameplay not started";
                    return false;
                }

                expectedRoomId = currentRoomId;
                expectedSessionId = sessionId;
                if (battleLoadEntryCaptured &&
                    battleLoadEntryRoomId == expectedRoomId &&
                    battleInstanceId != 0UL &&
                    ContainsSessionId(battleLoadPlayerSessionIds, expectedSessionId))
                {
                    lastError = string.Empty;
                    return true;
                }
            }

            try
            {
                byte[] packet = await ReceivePacketExclusiveAsync(CancellationToken.None);
                if (!PlayerTcpPacket.TryParseBattleLoadEntry(
                        packet,
                        out uint roomId,
                        out ulong nextBattleInstanceId,
                        out ulong[] playerSessionIds) ||
                    roomId == 0U ||
                    roomId != expectedRoomId ||
                    nextBattleInstanceId == 0UL ||
                    !ContainsSessionId(playerSessionIds, expectedSessionId))
                {
                    throw new InvalidOperationException("invalid battle load entry packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    battleLoadEntryCaptured = true;
                    battleLoadEntryRoomId = roomId;
                    battleInstanceId = nextBattleInstanceId;
                    battleLoadPlayerSessionIds = CopyUlongs(playerSessionIds);
                    ClearBattleFinalRankingState();
                    arenaLoadCompleteSent = false;
                    arenaGameplayStarted = false;
                    arenaGameplayStartRoomId = 0U;
                    arenaGameplayStartBattleInstanceId = 0UL;
                    ClearMonsterSpawnState();
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> SendArenaLoadCompleteAsync()
        {
            uint roomId;
            ulong currentBattleInstanceId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (!battleLoadEntryCaptured || battleLoadEntryRoomId == 0U || battleInstanceId == 0UL)
                {
                    lastError = "arena load entry not captured";
                    return false;
                }
                if (arenaLoadCompleteSent)
                {
                    lastError = string.Empty;
                    return true;
                }

                roomId = battleLoadEntryRoomId;
                currentBattleInstanceId = battleInstanceId;
            }

            try
            {
                byte[] packet = PlayerTcpPacket.SerializeArenaLoadComplete(roomId, currentBattleInstanceId);
                await SendTcpPacketAsync(packet, CancellationToken.None);
                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    arenaLoadCompleteSent = true;
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureArenaGameplayStartAsync()
        {
            uint expectedRoomId;
            ulong expectedBattleInstanceId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (!battleLoadEntryCaptured || battleLoadEntryRoomId == 0U || battleInstanceId == 0UL)
                {
                    lastError = "arena load entry not captured";
                    return false;
                }
                if (!arenaLoadCompleteSent)
                {
                    lastError = "arena load complete not sent";
                    return false;
                }

                expectedRoomId = battleLoadEntryRoomId;
                expectedBattleInstanceId = battleInstanceId;
                if (arenaGameplayStarted &&
                    arenaGameplayStartRoomId == expectedRoomId &&
                    arenaGameplayStartBattleInstanceId == expectedBattleInstanceId)
                {
                    lastError = string.Empty;
                    return true;
                }
            }

            try
            {
                byte[] packet = await ReceivePacketExclusiveAsync(
                    CancellationToken.None,
                    () => TryReturnCapturedArenaGameplayStart(
                        expectedRoomId,
                        expectedBattleInstanceId));
                if (packet == null)
                {
                    return true;
                }

                if (!PlayerTcpPacket.TryParseArenaGameplayStart(
                        packet,
                        out uint roomId,
                        out ulong nextBattleInstanceId) ||
                    roomId != expectedRoomId ||
                    nextBattleInstanceId != expectedBattleInstanceId)
                {
                    throw new InvalidOperationException("invalid arena gameplay start packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    arenaGameplayStarted = true;
                    arenaGameplayStartRoomId = roomId;
                    arenaGameplayStartBattleInstanceId = nextBattleInstanceId;
                    ClearMonsterSpawnState();
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureBattleFinalRankingAsync()
        {
            if (!TryPrepareBattleFinalRankingCapture(
                    out uint expectedRoomId,
                    out ulong expectedBattleInstanceId,
                    out ulong expectedSessionId))
            {
                return false;
            }

            if (TryReturnCapturedBattleFinalRanking(
                    expectedRoomId,
                    expectedBattleInstanceId,
                    expectedSessionId))
            {
                return true;
            }

            try
            {
                while (true)
                {
                    byte[] packet = await ReceivePacketExclusiveAsync(
                        CancellationToken.None,
                        () =>
                            TryReturnCapturedBattleFinalRanking(
                                expectedRoomId,
                                expectedBattleInstanceId,
                                expectedSessionId) ||
                            TryReturnCapturedLobbyReturn(expectedRoomId));
                    if (packet == null)
                    {
                        if (TryReturnCapturedBattleFinalRanking(
                                expectedRoomId,
                                expectedBattleInstanceId,
                                expectedSessionId))
                        {
                            return true;
                        }

                        if (TryReturnCapturedLobbyReturn(expectedRoomId))
                        {
                            lock (syncRoot)
                            {
                                if (lastError.Length == 0)
                                {
                                    lastError = "battle final ranking not received";
                                }
                            }
                            return false;
                        }

                        continue;
                    }

                    if (!TryApplyStateSnapshotPacket(packet))
                    {
                        throw new InvalidOperationException(
                            "invalid battle final ranking packet");
                    }

                    if (TryReturnCapturedBattleFinalRanking(
                            expectedRoomId,
                            expectedBattleInstanceId,
                            expectedSessionId))
                    {
                        return true;
                    }

                    if (TryReturnCapturedLobbyReturn(expectedRoomId))
                    {
                        lock (syncRoot)
                        {
                            if (lastError.Length == 0)
                            {
                                lastError = "battle final ranking not received";
                            }
                        }
                        return false;
                    }
                }
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureLobbyReturnVisibilityAsync()
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }

                expectedRoomId = currentRoomId;
                if (expectedRoomId == 0U &&
                    battleFinalRankingCaptured &&
                    battleFinalRanking.RoomId != 0U)
                {
                    expectedRoomId = battleFinalRanking.RoomId;
                }

                if (expectedRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }

                if (lobbyReturnCaptured && lobbyReturnPreviousRoomId == expectedRoomId)
                {
                    lastError = string.Empty;
                    return true;
                }
            }

            try
            {
                while (true)
                {
                    byte[] packet = await ReceivePacketExclusiveAsync(
                        CancellationToken.None,
                        () => TryReturnCapturedLobbyReturn(expectedRoomId));
                    if (packet == null)
                    {
                        if (TryReturnCapturedLobbyReturn(expectedRoomId))
                        {
                            return true;
                        }

                        continue;
                    }

                    if (!TryApplyStateSnapshotPacket(packet))
                    {
                        throw new InvalidOperationException(
                            "invalid lobby return visibility packet");
                    }

                    if (TryReturnCapturedLobbyReturn(expectedRoomId))
                    {
                        return true;
                    }
                }
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureMonsterSpawnAsync()
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!battleStarted)
                {
                    lastError = "battle not started";
                    return false;
                }
                if (battleLoadEntryCaptured && !arenaGameplayStarted)
                {
                    lastError = "arena gameplay not started";
                    return false;
                }

                expectedRoomId = currentRoomId;
                if (monsterSpawned &&
                    monsterSpawnRoomId == expectedRoomId &&
                    monsterId != 0U &&
                    monsterTypeId != 0U &&
                    monsterMaxHp != 0)
                {
                    lastError = string.Empty;
                    return true;
                }
            }

            try
            {
                byte[] packet = await ReceivePacketExclusiveAsync(CancellationToken.None);
                if (!PlayerTcpPacket.TryParseMonsterSpawn(
                        packet,
                        out uint roomId,
                        out uint nextMonsterId,
                        out uint nextMonsterTypeId,
                        out ushort nextMaxHp) ||
                    roomId == 0U ||
                    roomId != expectedRoomId ||
                    nextMonsterId == 0U ||
                    nextMonsterTypeId == 0U ||
                    nextMaxHp == 0)
                {
                    throw new InvalidOperationException("invalid monster spawn packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    monsterSpawned = true;
                    monsterSpawnRoomId = roomId;
                    monsterId = nextMonsterId;
                    monsterTypeId = nextMonsterTypeId;
                    monsterMaxHp = nextMaxHp;
                    ClearRudpAttackState();
                    ClearMonsterHealthSnapshotState();
                    ClearMonsterDeathState();
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureMonsterHealthSnapshotAsync()
        {
            uint expectedRoomId;
            uint expectedMonsterId;
            ushort expectedMaxHp;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!battleStarted)
                {
                    lastError = "battle not started";
                    return false;
                }
                if (!monsterSpawned || monsterId == 0U)
                {
                    lastError = "monster not spawned";
                    return false;
                }

                expectedRoomId = currentRoomId;
                expectedMonsterId = monsterId;
                expectedMaxHp = monsterMaxHp;
                if (monsterHealthSnapshotCaptured &&
                    monsterHealthRoomId == expectedRoomId &&
                    monsterHealthMonsterId == expectedMonsterId &&
                    monsterHealthMaxHp == expectedMaxHp &&
                    monsterCurrentHp <= monsterHealthMaxHp)
                {
                    lastError = string.Empty;
                    return true;
                }
            }

            try
            {
                byte[] packet = await ReceivePacketExclusiveAsync(CancellationToken.None);
                if (!PlayerTcpPacket.TryParseMonsterHealthSnapshot(
                        packet,
                        out uint roomId,
                        out uint nextMonsterId,
                        out ushort nextCurrentHp,
                        out ushort nextMaxHp) ||
                    roomId == 0U ||
                    roomId != expectedRoomId ||
                    nextMonsterId == 0U ||
                    nextMonsterId != expectedMonsterId ||
                    nextMaxHp == 0 ||
                    nextMaxHp != expectedMaxHp ||
                    nextCurrentHp > nextMaxHp)
                {
                    throw new InvalidOperationException("invalid monster health snapshot packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    monsterHealthSnapshotCaptured = true;
                    monsterHealthRoomId = roomId;
                    monsterHealthMonsterId = nextMonsterId;
                    monsterCurrentHp = nextCurrentHp;
                    monsterHealthMaxHp = nextMaxHp;
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureMonsterDeathAsync()
        {
            uint expectedRoomId;
            uint expectedMonsterId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!battleStarted)
                {
                    lastError = "battle not started";
                    return false;
                }
                if (!monsterSpawned || monsterId == 0U)
                {
                    lastError = "monster not spawned";
                    return false;
                }

                expectedRoomId = currentRoomId;
                expectedMonsterId = monsterId;
                if (monsterDeathCaptured &&
                    monsterDeathRoomId == expectedRoomId &&
                    monsterDeathMonsterId == expectedMonsterId)
                {
                    lastError = string.Empty;
                    return true;
                }
            }

            try
            {
                byte[] packet = await ReceivePacketExclusiveAsync(CancellationToken.None);
                if (!PlayerTcpPacket.TryParseMonsterDeath(
                        packet,
                        out uint roomId,
                        out uint nextMonsterId) ||
                    roomId == 0U ||
                    roomId != expectedRoomId ||
                    nextMonsterId == 0U ||
                    nextMonsterId != expectedMonsterId)
                {
                    throw new InvalidOperationException("invalid monster death packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    monsterDeathCaptured = true;
                    monsterDeathRoomId = roomId;
                    monsterDeathMonsterId = nextMonsterId;
                    ClearDropListSnapshotV2State();
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureDropListSnapshotV2Async()
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!battleStarted)
                {
                    lastError = "battle not started";
                    return false;
                }
                if (!monsterSpawned || monsterId == 0U)
                {
                    lastError = "monster not spawned";
                    return false;
                }
                if (!monsterDeathCaptured || monsterDeathMonsterId == 0U)
                {
                    lastError = "monster death not captured";
                    return false;
                }

                expectedRoomId = currentRoomId;
                if (dropListSnapshotV2Captured &&
                    dropListSnapshotV2.RoomId == expectedRoomId)
                {
                    lastError = string.Empty;
                    return true;
                }
            }

            try
            {
                byte[] packet = await ReceivePacketExclusiveAsync(CancellationToken.None);
                if (!PlayerTcpPacket.TryParseDropListSnapshotV2(
                        packet,
                        out PlayerDropListSnapshotV2 snapshot) ||
                    snapshot.RoomId == 0U ||
                    snapshot.RoomId != expectedRoomId)
                {
                    throw new InvalidOperationException("invalid drop list snapshot v2 packet");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected)
                    {
                        return false;
                    }

                    ClearRudpSpaceLootState();
                    dropListSnapshotV2Captured = true;
                    dropListSnapshotV2 = snapshot;
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> SendAttackIntentAsync()
        {
            PlayerServerEndpoint expectedEndpoint;
            ulong expectedSessionId;
            uint expectedMonsterId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (!rudpHelloSent || rudpHelloSessionId != sessionId)
                {
                    lastError = "RUDP Hello not sent";
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!battleStarted)
                {
                    lastError = "battle not started";
                    return false;
                }
                if (!monsterSpawned || monsterId == 0U)
                {
                    lastError = "monster not spawned";
                    return false;
                }

                expectedEndpoint = endpoint;
                expectedSessionId = sessionId;
                expectedMonsterId = monsterId;
            }

            try
            {
                PlayerRudpInputCommandSendResult result =
                    await rudpSender.SendAttackAsync(
                        expectedEndpoint,
                        expectedSessionId,
                        expectedMonsterId,
                        CancellationToken.None);
                if (!result.IsSent ||
                    result.SessionId != expectedSessionId ||
                    result.TargetHintMonsterId != expectedMonsterId)
                {
                    throw new InvalidOperationException("invalid RUDP Attack result");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected ||
                        sessionId != expectedSessionId)
                    {
                        return false;
                    }

                    rudpAttackSent = true;
                    rudpAttackSessionId = result.SessionId;
                    rudpAttackSequence = result.Sequence;
                    rudpAttackCommandSequence = result.CommandSequence;
                    rudpAttackTargetHintMonsterId = result.TargetHintMonsterId;
                    rudpAttackLocalEndpoint = result.LocalEndpoint;
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> SendMoveIntentAsync(PlayerInputIntent intent)
        {
            PlayerServerEndpoint expectedEndpoint;
            ulong expectedSessionId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (!rudpHelloSent || rudpHelloSessionId != sessionId)
                {
                    lastError = "RUDP Hello not sent";
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }

                expectedEndpoint = endpoint;
                expectedSessionId = sessionId;
            }

            try
            {
                PlayerRudpInputCommandSendResult result =
                    await rudpSender.SendMoveAsync(
                        expectedEndpoint,
                        expectedSessionId,
                        ScaleMoveAxis(intent.MoveX),
                        ScaleMoveAxis(intent.MoveZ),
                        CancellationToken.None);
                if (!result.IsSent || result.SessionId != expectedSessionId)
                {
                    throw new InvalidOperationException("invalid RUDP Move result");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected ||
                        sessionId != expectedSessionId)
                    {
                        return false;
                    }

                    rudpMoveSent = true;
                    rudpMoveSessionId = result.SessionId;
                    rudpMoveSequence = result.Sequence;
                    rudpMoveCommandSequence = result.CommandSequence;
                    rudpMoveLocalEndpoint = result.LocalEndpoint;
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public bool DrainIncomingRudpStateSnapshot()
        {
            uint expectedRoomId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U)
                {
                    return false;
                }

                expectedRoomId = currentRoomId;
            }

            if (!rudpSender.TryReceiveStateSnapshot(out PlayerRudpStateSnapshot snapshot) ||
                !snapshot.IsValid ||
                snapshot.RoomId != expectedRoomId)
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId != expectedRoomId)
                {
                    return false;
                }

                stateSnapshotCaptured = true;
                stateSnapshot = snapshot;
                lastError = string.Empty;
            }

            return true;
        }

        public async Task<bool> SendSpaceLootIntentAsync()
        {
            PlayerServerEndpoint expectedEndpoint;
            ulong expectedSessionId;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (!rudpHelloSent || rudpHelloSessionId != sessionId)
                {
                    lastError = "RUDP Hello not sent";
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!battleStarted)
                {
                    lastError = "battle not started";
                    return false;
                }
                if (!monsterSpawned || monsterId == 0U)
                {
                    lastError = "monster not spawned";
                    return false;
                }
                if (!monsterDeathCaptured || monsterDeathMonsterId == 0U)
                {
                    lastError = "monster death not captured";
                    return false;
                }
                if (!dropListSnapshotV2Captured)
                {
                    lastError = "drop list not captured";
                    return false;
                }

                expectedEndpoint = endpoint;
                expectedSessionId = sessionId;
            }

            try
            {
                PlayerRudpInputCommandSendResult result =
                    await rudpSender.SendSpaceLootAsync(
                        expectedEndpoint,
                        expectedSessionId,
                        CancellationToken.None);
                if (!result.IsSent || result.SessionId != expectedSessionId)
                {
                    throw new InvalidOperationException("invalid RUDP SpaceLoot result");
                }

                lock (syncRoot)
                {
                    if (status != PlayerNetworkSessionStatus.Connected ||
                        sessionId != expectedSessionId)
                    {
                        return false;
                    }

                    rudpSpaceLootSent = true;
                    rudpSpaceLootSessionId = result.SessionId;
                    rudpSpaceLootSequence = result.Sequence;
                    rudpSpaceLootCommandSequence = result.CommandSequence;
                    rudpSpaceLootLocalEndpoint = result.LocalEndpoint;
                    lastError = string.Empty;
                }

                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureLootResolvedAsync()
        {
            if (!TryPrepareLootResultCapture(
                    out uint expectedRoomId,
                    out ulong expectedSessionId))
            {
                return false;
            }

            if (TryReturnCapturedLootResolved(expectedRoomId, expectedSessionId))
            {
                return true;
            }

            try
            {
                using (CancellationTokenSource cancellation =
                    new CancellationTokenSource(LootCaptureTimeoutMilliseconds))
                {
                    while (true)
                    {
                        byte[] packet = await ReceivePacketExclusiveAsync(cancellation.Token);
                        if (TryApplyStateSnapshotPacket(packet))
                        {
                            if (TryReturnCapturedLootResolved(expectedRoomId, expectedSessionId))
                            {
                                return true;
                            }

                            continue;
                        }

                        throw new InvalidOperationException("invalid loot resolved packet");
                    }
                }
            }
            catch (OperationCanceledException)
            {
                MarkCommandReceiveTimedOut("loot resolved receive timeout");
                return false;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> CaptureLootRejectedAsync()
        {
            if (!TryPrepareLootResultCapture(
                    out uint expectedRoomId,
                    out ulong expectedSessionId))
            {
                return false;
            }

            if (TryReturnCapturedLootRejected(expectedRoomId, expectedSessionId))
            {
                return true;
            }

            if (TryRejectLocalWinnerLootRejectedCapture(expectedRoomId, expectedSessionId))
            {
                return false;
            }

            try
            {
                using (CancellationTokenSource cancellation =
                    new CancellationTokenSource(LootCaptureTimeoutMilliseconds))
                {
                    while (true)
                    {
                        byte[] packet = await ReceivePacketExclusiveAsync(cancellation.Token);
                        if (!PlayerTcpPacket.TryParseLootRejected(
                                packet,
                                out PlayerLootRejected result))
                        {
                            if (TryApplyStateSnapshotPacket(packet))
                            {
                                if (TryReturnCapturedLootRejected(
                                        expectedRoomId,
                                        expectedSessionId))
                                {
                                    return true;
                                }

                                continue;
                            }

                            if (PlayerTcpPacket.TryParseError(
                                    packet,
                                    out ushort failedType,
                                    out ushort errorCode) &&
                                failedType == PlayerTcpPacket.ClickLootRequestPacketType)
                            {
                                MarkRoomCommandRejected("space loot", errorCode);
                                return false;
                            }

                            throw new InvalidOperationException("invalid loot rejected packet");
                        }

                        if (result.RoomId != expectedRoomId)
                        {
                            throw new InvalidOperationException("invalid loot rejected packet");
                        }

                        lock (syncRoot)
                        {
                            if (status != PlayerNetworkSessionStatus.Connected ||
                                sessionId != expectedSessionId)
                            {
                                return false;
                            }
                            if (currentRoomId != expectedRoomId || result.DropId == 0U)
                            {
                                throw new InvalidOperationException(
                                    "invalid loot rejected packet");
                            }

                            lootRejectedCaptured = true;
                            lootRejected = result;
                            lastError = string.Empty;
                        }

                        return true;
                    }
                }
            }
            catch (OperationCanceledException)
            {
                MarkCommandReceiveTimedOut("loot rejected receive timeout");
                return false;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        private bool TryRejectLocalWinnerLootRejectedCapture(
            uint expectedRoomId,
            ulong expectedSessionId)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    sessionId != expectedSessionId ||
                    !lootResolvedCaptured ||
                    lootResolved.RoomId != expectedRoomId ||
                    lootResolved.WinnerSessionId != expectedSessionId)
                {
                    return false;
                }

                lastError = "loot winner is local session";
                return true;
            }
        }

        public async Task<bool> CaptureInventorySnapshotAsync()
        {
            if (!TryPrepareInventorySnapshotCapture(
                    out ulong expectedSessionId,
                    out PlayerLootResolved expectedLootResolved))
            {
                return false;
            }

            if (TryReturnCapturedInventorySnapshot(
                    expectedSessionId,
                    expectedLootResolved))
            {
                return true;
            }

            try
            {
                using (CancellationTokenSource cancellation =
                    new CancellationTokenSource(LootCaptureTimeoutMilliseconds))
                {
                    while (true)
                    {
                        byte[] packet = await ReceivePacketExclusiveAsync(cancellation.Token);
                        if (TryApplyExpectedInventorySnapshotPacket(
                                packet,
                                expectedSessionId,
                                expectedLootResolved))
                        {
                            if (TryReturnCapturedInventorySnapshot(
                                    expectedSessionId,
                                    expectedLootResolved))
                            {
                                return true;
                            }

                            continue;
                        }

                        if (TryApplyStateSnapshotPacket(packet))
                        {
                            if (TryReturnCapturedInventorySnapshot(
                                    expectedSessionId,
                                    expectedLootResolved))
                            {
                                return true;
                            }

                            continue;
                        }

                        throw new InvalidOperationException("invalid inventory snapshot packet");
                    }
                }
            }
            catch (OperationCanceledException)
            {
                MarkCommandReceiveTimedOut("inventory snapshot receive timeout");
                return false;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public Task<bool> SendCreateRoomRequestAsync()
        {
            return SendCreateRoomRequestAsync("Room", PlayerTcpPacket.CreateRoomMaxCapacity);
        }

        public async Task<bool> SendHeartbeatAsync()
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeHeartbeatRequest(),
                    CancellationToken.None);
                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public async Task<bool> SendCreateRoomRequestAsync(string roomTitle, int maxPlayers)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
            }

            try
            {
                await SendTcpPacketAsync(
                    PlayerTcpPacket.SerializeCreateRoomRequest(roomTitle, maxPlayers),
                    CancellationToken.None);
                return true;
            }
            catch (Exception ex)
            {
                MarkCommandSendFailed(ex);
                return false;
            }
        }

        public void Disconnect()
        {
            CancellationTokenSource cancellationToStop;
            lock (syncRoot)
            {
                cancellationToStop = connectCancellation;
                connectCancellation = null;
                status = PlayerNetworkSessionStatus.Disconnected;
                sessionId = 0UL;
                ClearDeferredTcpPacketState();
                ClearClientListSnapshotState();
                ClearRudpHelloState();
                ClearRoomState();
                lastError = string.Empty;
            }

            if (cancellationToStop != null)
            {
                cancellationToStop.Cancel();
                cancellationToStop.Dispose();
            }

            connector.Disconnect();
            rudpSender.Disconnect();
        }

        private void DisconnectBeforeReconnectIfNeeded()
        {
            CancellationTokenSource cancellationToStop;
            bool shouldDisconnectConnector;
            lock (syncRoot)
            {
                cancellationToStop = connectCancellation;
                connectCancellation = null;
                shouldDisconnectConnector =
                    status == PlayerNetworkSessionStatus.Connecting ||
                    status == PlayerNetworkSessionStatus.Connected;
                status = PlayerNetworkSessionStatus.Disconnected;
                sessionId = 0UL;
                ClearDeferredTcpPacketState();
                ClearClientListSnapshotState();
                ClearRudpHelloState();
                ClearRoomState();
                lastError = string.Empty;
            }

            if (cancellationToStop != null)
            {
                cancellationToStop.Cancel();
                cancellationToStop.Dispose();
            }

            if (shouldDisconnectConnector)
            {
                connector.Disconnect();
                rudpSender.Disconnect();
            }
        }

        private void MarkCommandSendFailed(Exception ex)
        {
            bool shouldDisconnect = false;
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return;
                }

                status = PlayerNetworkSessionStatus.Failed;
                sessionId = 0UL;
                ClearDeferredTcpPacketState();
                ClearClientListSnapshotState();
                ClearRudpHelloState();
                ClearRoomState();
                lastError = ex.Message;
                shouldDisconnect = true;
            }

            if (shouldDisconnect)
            {
                connector.Disconnect();
                rudpSender.Disconnect();
            }
        }

        private void MarkCommandReceiveTimedOut(string error)
        {
            lock (syncRoot)
            {
                if (status == PlayerNetworkSessionStatus.Connected)
                {
                    lastError = error;
                }
            }
        }

        private async Task SendTcpPacketAsync(byte[] packet, CancellationToken cancellationToken)
        {
            await tcpSendSemaphore.WaitAsync(cancellationToken);
            try
            {
                await connector.SendPacketAsync(packet, cancellationToken);
            }
            finally
            {
                tcpSendSemaphore.Release();
            }
        }

        private async Task<byte[]> ReceiveNextCommandResponsePacketAsync(
            CancellationToken cancellationToken)
        {
            while (true)
            {
                byte[] packet = await ReceivePacketExclusiveAsync(cancellationToken);
                if (TryApplyStateSnapshotPacket(packet))
                {
                    ThrowIfDisconnectedByStatePacket();
                    continue;
                }

                return packet;
            }
        }

        private async Task<PlayerRoomListSnapshot> ReceiveRoomListSnapshotAsync(
            CancellationToken cancellationToken)
        {
            while (true)
            {
                byte[] packet = await ReceivePacketExclusiveAsync(cancellationToken);
                if (PlayerTcpPacket.TryParseRoomListSnapshot(
                        packet,
                        out PlayerRoomListSnapshot snapshot))
                {
                    ApplyRoomListSnapshot(snapshot);
                    return snapshot;
                }

                if (TryApplyClientListSnapshotPacket(packet))
                {
                    continue;
                }

                throw new InvalidOperationException("invalid room list snapshot packet");
            }
        }

        private async Task ReceiveRoomVisibilityAfterCommandAsync(
            uint expectedRoomId,
            ushort expectedPlayerCount,
            CancellationToken cancellationToken)
        {
            SetRoomVisibilityCommandInFlight(true);
            try
            {
                while (true)
                {
                    byte[] packet = await ReceivePacketExclusiveAsync(cancellationToken);
                    if (PlayerTcpPacket.TryParseRoomDetailState(
                            packet,
                            out PlayerRoomDetailState detail))
                    {
                        if (detail.RoomId != expectedRoomId ||
                            detail.MemberCount != expectedPlayerCount)
                        {
                            throw new InvalidOperationException("invalid room detail state packet");
                        }

                        ApplyRoomDetailState(detail);
                        return;
                    }

                    if (PlayerTcpPacket.TryParseRoomListSnapshot(
                            packet,
                            out PlayerRoomListSnapshot snapshot))
                    {
                        ApplyRoomListSnapshot(snapshot);
                        continue;
                    }

                    if (TryApplyClientListSnapshotPacket(packet))
                    {
                        continue;
                    }

                    throw new InvalidOperationException("invalid room visibility packet");
                }
            }
            finally
            {
                SetRoomVisibilityCommandInFlight(false);
            }
        }

        private async Task DrainPendingTcpStateSnapshotsAsync()
        {
            while (connector.HasPendingPacket)
            {
                byte[] packet = await ReceivePacketExclusiveAsync(CancellationToken.None);
                if (!TryApplyStateSnapshotPacket(packet))
                {
                    EnqueueDeferredTcpPacket(packet);
                    return;
                }
            }
        }

        private Task<byte[]> ReceivePacketExclusiveAsync(
            CancellationToken cancellationToken)
        {
            return ReceivePacketExclusiveAsync(cancellationToken, null);
        }

        private async Task<byte[]> ReceivePacketExclusiveAsync(
            CancellationToken cancellationToken,
            Func<bool> shouldSkipReceive)
        {
            if (TryDequeueDeferredTcpPacket(out byte[] deferredPacket))
            {
                return deferredPacket;
            }

            await tcpReceiveSemaphore.WaitAsync(cancellationToken);
            try
            {
                if (TryDequeueDeferredTcpPacket(out deferredPacket))
                {
                    return deferredPacket;
                }

                if (shouldSkipReceive != null && shouldSkipReceive())
                {
                    return null;
                }

                return await connector.ReceivePacketAsync(cancellationToken);
            }
            finally
            {
                tcpReceiveSemaphore.Release();
            }
        }

        private void EnqueueDeferredTcpPacket(byte[] packet)
        {
            if (packet == null)
            {
                return;
            }

            lock (syncRoot)
            {
                deferredTcpPackets.Enqueue(packet);
            }
        }

        private bool TryDequeueDeferredTcpPacket(out byte[] packet)
        {
            lock (syncRoot)
            {
                if (deferredTcpPackets.Count == 0)
                {
                    packet = null;
                    return false;
                }

                packet = deferredTcpPackets.Dequeue();
                return true;
            }
        }

        private bool TryApplyStateSnapshotPacket(byte[] packet)
        {
            if (TryApplySessionReplacedPacket(packet))
            {
                return true;
            }

            if (TryApplyClientListSnapshotPacket(packet))
            {
                return true;
            }

            if (PlayerTcpPacket.TryParseRoomListSnapshot(
                    packet,
                    out PlayerRoomListSnapshot snapshot))
            {
                ApplyRoomListSnapshot(snapshot);
                return true;
            }

            if (TryApplyRoomDetailStatePacket(packet))
            {
                return true;
            }

            if (TryApplyBattleFinalRankingPacket(packet))
            {
                return true;
            }

            if (TryApplyLobbyReturnVisibilityPacket(packet))
            {
                return true;
            }

            if (TryApplyBattleStartPacket(packet))
            {
                return true;
            }

            if (TryApplyBattleLoadEntryPacket(packet))
            {
                return true;
            }

            if (TryApplyArenaGameplayStartPacket(packet))
            {
                return true;
            }

            if (TryApplyMonsterSpawnPacket(packet))
            {
                return true;
            }

            if (TryApplyMonsterHealthSnapshotPacket(packet))
            {
                return true;
            }

            if (TryApplyMonsterDeathPacket(packet))
            {
                return true;
            }

            if (TryApplyDropListSnapshotV2Packet(packet))
            {
                return true;
            }

            if (TryApplyLootResolvedPacket(packet))
            {
                return true;
            }

            if (TryApplyLootRejectedPacket(packet))
            {
                return true;
            }

            if (TryApplyInventorySnapshotPacket(packet))
            {
                return true;
            }

            return false;
        }

        private bool TryApplySessionReplacedPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseSessionReplaced(packet))
            {
                return false;
            }

            MarkSessionReplaced();
            return true;
        }

        private void MarkSessionReplaced()
        {
            bool shouldDisconnect = false;
            lock (syncRoot)
            {
                if (status == PlayerNetworkSessionStatus.Connected ||
                    status == PlayerNetworkSessionStatus.Connecting)
                {
                    shouldDisconnect = true;
                }

                status = PlayerNetworkSessionStatus.Disconnected;
                sessionId = 0UL;
                ClearDeferredTcpPacketState();
                ClearClientListSnapshotState();
                ClearRudpHelloState();
                ClearRoomState();
                lastError = SessionReplacedMessage;
            }

            if (shouldDisconnect)
            {
                connector.Disconnect();
                rudpSender.Disconnect();
            }
        }

        private void ThrowIfDisconnectedByStatePacket()
        {
            lock (syncRoot)
            {
                if (status == PlayerNetworkSessionStatus.Connected)
                {
                    return;
                }

                throw new InvalidOperationException(
                    string.IsNullOrEmpty(lastError) ?
                        "session disconnected" :
                        lastError);
            }
        }

        private bool TryApplyClientListSnapshotPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseClientListSnapshot(
                    packet,
                    out PlayerClientListSnapshot snapshot))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status == PlayerNetworkSessionStatus.Connected)
                {
                    clientListSnapshot = snapshot;
                }
            }

            return true;
        }

        private bool TryApplyRoomDetailStatePacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseRoomDetailState(
                    packet,
                    out PlayerRoomDetailState detail))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    currentRoomId != detail.RoomId)
                {
                    return !roomVisibilityCommandInFlight;
                }
            }

            ApplyRoomDetailState(detail);
            return true;
        }

        private void SetRoomVisibilityCommandInFlight(bool inFlight)
        {
            lock (syncRoot)
            {
                roomVisibilityCommandInFlight = inFlight;
            }
        }

        private bool TryApplyBattleFinalRankingPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseBattleFinalRanking(
                    packet,
                    out PlayerBattleFinalRanking ranking))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    !battleLoadEntryCaptured ||
                    !arenaGameplayStarted ||
                    ranking.RoomId != currentRoomId ||
                    ranking.BattleInstanceId != battleInstanceId ||
                    !RankingContainsSessionId(ranking, sessionId))
                {
                    return false;
                }

                battleFinalRankingCaptured = true;
                battleFinalRanking = ranking;
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyLobbyReturnVisibilityPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseLobbyReturnVisibility(
                    packet,
                    out uint previousRoomId,
                    out PlayerLobbyReturnReason reason))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    (currentRoomId != 0U && currentRoomId != previousRoomId))
                {
                    return false;
                }

                ClearRoomState(preserveBattleFinalRanking: true);
                lobbyReturnCaptured = true;
                lobbyReturnPreviousRoomId = previousRoomId;
                lobbyReturnReason = reason;
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyMonsterDeathPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseMonsterDeath(
                    packet,
                    out uint roomId,
                    out uint nextMonsterId))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    !battleStarted ||
                    !monsterSpawned ||
                    roomId == 0U ||
                    roomId != currentRoomId ||
                    nextMonsterId == 0U ||
                    nextMonsterId != monsterId)
                {
                    return false;
                }

                monsterDeathCaptured = true;
                monsterDeathRoomId = roomId;
                monsterDeathMonsterId = nextMonsterId;
                if (monsterHealthSnapshotCaptured &&
                    monsterHealthMonsterId == nextMonsterId)
                {
                    monsterCurrentHp = 0;
                }
                ClearDropListSnapshotV2State();
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyDropListSnapshotV2Packet(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseDropListSnapshotV2(
                    packet,
                    out PlayerDropListSnapshotV2 snapshot))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    !battleStarted ||
                    !monsterSpawned ||
                    !monsterDeathCaptured ||
                    snapshot.RoomId == 0U ||
                    snapshot.RoomId != currentRoomId)
                {
                    return false;
                }

                dropListSnapshotV2Captured = true;
                dropListSnapshotV2 = snapshot;
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyLootResolvedPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseLootResolved(
                    packet,
                    out PlayerLootResolved result))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    result.RoomId != currentRoomId ||
                    result.DropId == 0U ||
                    result.WinnerSessionId == 0UL ||
                    result.ItemId == 0U ||
                    result.Quantity == 0 ||
                    !CanApplyLootResolvedFromCurrentDropState(
                        result.DropId,
                        result.ItemId,
                        result.Quantity))
                {
                    return false;
                }

                lootResolvedCaptured = true;
                lootResolved = result;
                ClearInventorySnapshotState();
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyLootRejectedPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseLootRejected(
                    packet,
                    out PlayerLootRejected result))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    result.RoomId != currentRoomId ||
                    !dropListSnapshotV2Captured ||
                    !rudpSpaceLootSent ||
                    rudpSpaceLootSessionId != sessionId ||
                    result.DropId == 0U)
                {
                    return false;
                }

                lootRejectedCaptured = true;
                lootRejected = result;
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyInventorySnapshotPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseInventorySnapshot(
                    packet,
                    out PlayerInventorySnapshot snapshot))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    snapshot.SessionId != sessionId ||
                    !lootResolvedCaptured ||
                    lootResolved.WinnerSessionId != sessionId ||
                    !InventoryContainsResolvedItem(
                        snapshot,
                        lootResolved.ItemId,
                        lootResolved.Quantity))
                {
                    return false;
                }

                inventorySnapshotCaptured = true;
                inventorySnapshot = snapshot;
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyExpectedInventorySnapshotPacket(
            byte[] packet,
            ulong expectedSessionId,
            PlayerLootResolved expectedLootResolved)
        {
            if (!PlayerTcpPacket.TryParseInventorySnapshot(
                    packet,
                    out PlayerInventorySnapshot snapshot))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    sessionId != expectedSessionId ||
                    expectedLootResolved.WinnerSessionId != expectedSessionId ||
                    expectedLootResolved.DropId == 0U ||
                    expectedLootResolved.ItemId == 0U ||
                    expectedLootResolved.Quantity == 0 ||
                    snapshot.SessionId != expectedSessionId ||
                    !InventoryContainsResolvedItem(
                        snapshot,
                        expectedLootResolved.ItemId,
                        expectedLootResolved.Quantity))
                {
                    return false;
                }

                inventorySnapshotCaptured = true;
                inventorySnapshot = snapshot;
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyMonsterHealthSnapshotPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseMonsterHealthSnapshot(
                    packet,
                    out uint roomId,
                    out uint nextMonsterId,
                    out ushort nextCurrentHp,
                    out ushort nextMaxHp))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    !battleStarted ||
                    !monsterSpawned ||
                    roomId == 0U ||
                    roomId != currentRoomId ||
                    nextMonsterId == 0U ||
                    nextMonsterId != monsterId ||
                    nextMaxHp == 0 ||
                    nextMaxHp != monsterMaxHp ||
                    nextCurrentHp > nextMaxHp)
                {
                    return false;
                }

                monsterHealthSnapshotCaptured = true;
                monsterHealthRoomId = roomId;
                monsterHealthMonsterId = nextMonsterId;
                monsterCurrentHp = nextCurrentHp;
                monsterHealthMaxHp = nextMaxHp;
                lastError = string.Empty;
            }

            return true;
        }

        private void ApplyRoomListSnapshot(PlayerRoomListSnapshot snapshot)
        {
            lock (syncRoot)
            {
                if (status == PlayerNetworkSessionStatus.Connected)
                {
                    roomListSnapshot = snapshot;
                }
            }
        }

        private void ApplyRoomDetailState(PlayerRoomDetailState detail)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return;
                }

                roomDetailCaptured = true;
                roomDetailState = detail;
                currentRoomId = detail.RoomId;
                currentRoomPlayerCount = (ushort)detail.MemberCount;
                currentRoomReadyPlayerCount = CountReadyMembers(detail);
                if (detail.Status == PlayerRoomStatus.InProgress)
                {
                    battleStarted = true;
                    battleStartRoomId = detail.RoomId;
                }
                else
                {
                    ClearBattleState();
                }
                lastError = string.Empty;
            }
        }

        private bool TryApplyBattleStartPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseBattleStart(
                    packet,
                    out uint roomId,
                    out ulong playerASessionId,
                    out ulong playerBSessionId))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    roomId == 0U ||
                    roomId != currentRoomId ||
                    playerASessionId == 0UL ||
                    playerBSessionId == 0UL ||
                    playerASessionId == playerBSessionId ||
                    (playerASessionId != sessionId && playerBSessionId != sessionId))
                {
                    return false;
                }

                battleStarted = true;
                battleStartRoomId = roomId;
                battleStartPlayerASessionId = playerASessionId;
                battleStartPlayerBSessionId = playerBSessionId;
                ClearBattleFinalRankingState();
                ClearArenaLoadState();
                ClearMonsterSpawnState();
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyBattleLoadEntryPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseBattleLoadEntry(
                    packet,
                    out uint roomId,
                    out ulong nextBattleInstanceId,
                    out ulong[] playerSessionIds))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    !battleStarted ||
                    roomId == 0U ||
                    roomId != currentRoomId ||
                    nextBattleInstanceId == 0UL ||
                    !ContainsSessionId(playerSessionIds, sessionId))
                {
                    return false;
                }

                battleLoadEntryCaptured = true;
                battleLoadEntryRoomId = roomId;
                battleInstanceId = nextBattleInstanceId;
                battleLoadPlayerSessionIds = CopyUlongs(playerSessionIds);
                ClearBattleFinalRankingState();
                arenaLoadCompleteSent = false;
                arenaGameplayStarted = false;
                arenaGameplayStartRoomId = 0U;
                arenaGameplayStartBattleInstanceId = 0UL;
                ClearMonsterSpawnState();
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyArenaGameplayStartPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseArenaGameplayStart(
                    packet,
                    out uint roomId,
                    out ulong nextBattleInstanceId))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    !battleLoadEntryCaptured ||
                    roomId == 0U ||
                    roomId != battleLoadEntryRoomId ||
                    nextBattleInstanceId == 0UL ||
                    nextBattleInstanceId != battleInstanceId)
                {
                    return false;
                }

                arenaGameplayStarted = true;
                arenaGameplayStartRoomId = roomId;
                arenaGameplayStartBattleInstanceId = nextBattleInstanceId;
                ClearMonsterSpawnState();
                lastError = string.Empty;
            }

            return true;
        }

        private bool TryApplyMonsterSpawnPacket(byte[] packet)
        {
            if (!PlayerTcpPacket.TryParseMonsterSpawn(
                    packet,
                    out uint roomId,
                    out uint nextMonsterId,
                    out uint nextMonsterTypeId,
                    out ushort nextMaxHp))
            {
                return false;
            }

            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    currentRoomId == 0U ||
                    !battleStarted ||
                    (battleLoadEntryCaptured && !arenaGameplayStarted) ||
                    roomId == 0U ||
                    roomId != currentRoomId ||
                    nextMonsterId == 0U ||
                    nextMonsterTypeId == 0U ||
                    nextMaxHp == 0)
                {
                    return false;
                }

                monsterSpawned = true;
                monsterSpawnRoomId = roomId;
                monsterId = nextMonsterId;
                monsterTypeId = nextMonsterTypeId;
                monsterMaxHp = nextMaxHp;
                ClearRudpAttackState();
                ClearMonsterHealthSnapshotState();
                ClearMonsterDeathState();
                lastError = string.Empty;
            }

            return true;
        }

        private void MarkRoomListCaptureUnavailable()
        {
            lock (syncRoot)
            {
                if (status == PlayerNetworkSessionStatus.Connected)
                {
                    lastError = "room list snapshot not available";
                }
            }
        }

        private void MarkRoomCommandRejected(string commandName, ushort errorCode)
        {
            lock (syncRoot)
            {
                if (status == PlayerNetworkSessionStatus.Connected)
                {
                    lastError = commandName + " rejected: " + TcpErrorName(errorCode);
                }
            }
        }

        private static string TcpErrorName(ushort errorCode)
        {
            switch (errorCode)
            {
                case PlayerTcpPacket.ErrorCodeFull:
                    return "Full";
                case PlayerTcpPacket.ErrorCodeNotFound:
                    return "NotFound";
                case PlayerTcpPacket.ErrorCodeAlreadyInRoom:
                    return "AlreadyInRoom";
                case PlayerTcpPacket.ErrorCodeNotInRoom:
                    return "NotInRoom";
                case PlayerTcpPacket.ErrorCodeAlreadyStarted:
                    return "AlreadyStarted";
                case PlayerTcpPacket.ErrorCodeNotHost:
                    return "NotHost";
                case PlayerTcpPacket.ErrorCodeNotAllReady:
                    return "NotAllReady";
                case PlayerTcpPacket.ErrorCodeNotEnoughPlayers:
                    return "NotEnoughPlayers";
                case PlayerTcpPacket.ErrorCodeInvalidTarget:
                    return "InvalidTarget";
                default:
                    return "Unknown";
            }
        }

        private ushort CountReadyMembers()
        {
            return roomDetailCaptured ? CountReadyMembers(roomDetailState) : currentRoomReadyPlayerCount;
        }

        private static ushort CountReadyMembers(PlayerRoomDetailState detail)
        {
            ushort readyCount = 0;
            for (int index = 0; index < detail.MemberCount; ++index)
            {
                if (detail.MemberAt(index).Ready)
                {
                    ++readyCount;
                }
            }

            return readyCount;
        }

        private static short ScaleMoveAxis(short axis)
        {
            if (axis > 0)
            {
                return 1000;
            }

            if (axis < 0)
            {
                return -1000;
            }

            return 0;
        }

        private void ClearRudpHelloState()
        {
            rudpHelloSent = false;
            rudpHelloSessionId = 0UL;
            rudpHelloSequence = 0U;
            rudpHelloLocalEndpoint = string.Empty;
            ClearRudpMoveState();
            ClearRudpAttackState();
            ClearRudpSpaceLootState();
        }

        private void ClearClientListSnapshotState()
        {
            clientListSnapshot = default;
        }

        private void ClearDeferredTcpPacketState()
        {
            deferredTcpPackets.Clear();
        }

        private void ClearRoomState(bool preserveBattleFinalRanking = false)
        {
            currentRoomId = 0U;
            currentRoomPlayerCount = 0;
            currentRoomReadyPlayerCount = 0;
            roomDetailCaptured = false;
            roomDetailState = default;
            ClearLobbyReturnState();
            ClearRudpMoveState();
            ClearStateSnapshotState();
            ClearBattleState(preserveBattleFinalRanking);
            roomListSnapshot = default;
        }

        private void ClearBattleState(bool preserveBattleFinalRanking = false)
        {
            ClearLobbyReturnState();
            battleStarted = false;
            battleStartRoomId = 0U;
            battleStartPlayerASessionId = 0UL;
            battleStartPlayerBSessionId = 0UL;
            ClearArenaLoadState();
            ClearMonsterSpawnState();
            if (!preserveBattleFinalRanking)
            {
                ClearBattleFinalRankingState();
            }
        }

        private void ClearLobbyReturnState()
        {
            lobbyReturnCaptured = false;
            lobbyReturnPreviousRoomId = 0U;
            lobbyReturnReason = PlayerLobbyReturnReason.None;
        }

        private void ClearArenaLoadState()
        {
            battleLoadEntryCaptured = false;
            battleLoadEntryRoomId = 0U;
            battleInstanceId = 0UL;
            battleLoadPlayerSessionIds = Array.Empty<ulong>();
            arenaLoadCompleteSent = false;
            arenaGameplayStarted = false;
            arenaGameplayStartRoomId = 0U;
            arenaGameplayStartBattleInstanceId = 0UL;
        }

        private void ClearMonsterSpawnState()
        {
            monsterSpawned = false;
            monsterSpawnRoomId = 0U;
            monsterId = 0U;
            monsterTypeId = 0U;
            monsterMaxHp = 0;
            ClearRudpAttackState();
            ClearMonsterHealthSnapshotState();
            ClearMonsterDeathState();
        }

        private void ClearRudpAttackState()
        {
            rudpAttackSent = false;
            rudpAttackSessionId = 0UL;
            rudpAttackSequence = 0U;
            rudpAttackCommandSequence = 0U;
            rudpAttackTargetHintMonsterId = 0U;
            rudpAttackLocalEndpoint = string.Empty;
        }

        private void ClearRudpMoveState()
        {
            rudpMoveSent = false;
            rudpMoveSessionId = 0UL;
            rudpMoveSequence = 0U;
            rudpMoveCommandSequence = 0U;
            rudpMoveLocalEndpoint = string.Empty;
        }

        private void ClearStateSnapshotState()
        {
            stateSnapshotCaptured = false;
            stateSnapshot = default;
        }

        private void ClearMonsterHealthSnapshotState()
        {
            monsterHealthSnapshotCaptured = false;
            monsterHealthRoomId = 0U;
            monsterHealthMonsterId = 0U;
            monsterCurrentHp = 0;
            monsterHealthMaxHp = 0;
        }

        private void ClearMonsterDeathState()
        {
            monsterDeathCaptured = false;
            monsterDeathRoomId = 0U;
            monsterDeathMonsterId = 0U;
            ClearDropListSnapshotV2State();
        }

        private void ClearDropListSnapshotV2State()
        {
            dropListSnapshotV2Captured = false;
            dropListSnapshotV2 = default;
            ClearRudpSpaceLootState();
        }

        private void ClearRudpSpaceLootState()
        {
            rudpSpaceLootSent = false;
            rudpSpaceLootSessionId = 0UL;
            rudpSpaceLootSequence = 0U;
            rudpSpaceLootCommandSequence = 0U;
            rudpSpaceLootLocalEndpoint = string.Empty;
            ClearLootResultState();
        }

        private void ClearLootResultState()
        {
            lootResolvedCaptured = false;
            lootResolved = default;
            lootRejectedCaptured = false;
            lootRejected = default;
            ClearInventorySnapshotState();
        }

        private void ClearInventorySnapshotState()
        {
            inventorySnapshotCaptured = false;
            inventorySnapshot = default;
        }

        private void ClearBattleFinalRankingState()
        {
            battleFinalRankingCaptured = false;
            battleFinalRanking = default;
        }

        private bool TryPrepareBattleFinalRankingCapture(
            out uint expectedRoomId,
            out ulong expectedBattleInstanceId,
            out ulong expectedSessionId)
        {
            lock (syncRoot)
            {
                expectedRoomId = 0U;
                expectedBattleInstanceId = 0UL;
                expectedSessionId = 0UL;
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!battleLoadEntryCaptured || battleInstanceId == 0UL)
                {
                    lastError = "arena load entry not captured";
                    return false;
                }
                if (!arenaGameplayStarted)
                {
                    lastError = "arena gameplay not started";
                    return false;
                }

                expectedRoomId = currentRoomId;
                expectedBattleInstanceId = battleInstanceId;
                expectedSessionId = sessionId;
                return true;
            }
        }

        private bool TryReturnCapturedBattleFinalRanking(
            uint expectedRoomId,
            ulong expectedBattleInstanceId,
            ulong expectedSessionId)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    !battleFinalRankingCaptured ||
                    battleFinalRanking.RoomId != expectedRoomId ||
                    battleFinalRanking.BattleInstanceId != expectedBattleInstanceId ||
                    !RankingContainsSessionId(battleFinalRanking, expectedSessionId))
                {
                    return false;
                }

                lastError = string.Empty;
                return true;
            }
        }

        private bool TryReturnCapturedLobbyReturn(uint expectedRoomId)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    !lobbyReturnCaptured ||
                    lobbyReturnPreviousRoomId != expectedRoomId)
                {
                    return false;
                }

                lastError = string.Empty;
                return true;
            }
        }

        private bool TryReturnCapturedArenaGameplayStart(
            uint expectedRoomId,
            ulong expectedBattleInstanceId)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    !arenaGameplayStarted ||
                    arenaGameplayStartRoomId != expectedRoomId ||
                    arenaGameplayStartBattleInstanceId != expectedBattleInstanceId)
                {
                    return false;
                }

                lastError = string.Empty;
                return true;
            }
        }

        private bool TryPrepareLootResultCapture(
            out uint expectedRoomId,
            out ulong expectedSessionId)
        {
            lock (syncRoot)
            {
                expectedRoomId = 0U;
                expectedSessionId = 0UL;
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!dropListSnapshotV2Captured)
                {
                    lastError = "drop list not captured";
                    return false;
                }
                if (!rudpSpaceLootSent || rudpSpaceLootSessionId != sessionId)
                {
                    lastError = "space loot not sent";
                    return false;
                }

                expectedRoomId = currentRoomId;
                expectedSessionId = sessionId;
                return true;
            }
        }

        private bool TryPrepareInventorySnapshotCapture(
            out ulong expectedSessionId,
            out PlayerLootResolved expectedLootResolved)
        {
            lock (syncRoot)
            {
                expectedSessionId = 0UL;
                expectedLootResolved = default;
                if (status != PlayerNetworkSessionStatus.Connected)
                {
                    return false;
                }
                if (currentRoomId == 0U)
                {
                    lastError = "not in room";
                    return false;
                }
                if (!dropListSnapshotV2Captured)
                {
                    lastError = "drop list not captured";
                    return false;
                }
                if (!rudpSpaceLootSent || rudpSpaceLootSessionId != sessionId)
                {
                    lastError = "space loot not sent";
                    return false;
                }
                if (!lootResolvedCaptured)
                {
                    lastError = "loot resolved not captured";
                    return false;
                }
                if (lootResolved.WinnerSessionId != sessionId)
                {
                    lastError = "loot winner is not local session";
                    return false;
                }

                expectedSessionId = sessionId;
                expectedLootResolved = lootResolved;
                return true;
            }
        }

        private bool TryReturnCapturedLootResolved(
            uint expectedRoomId,
            ulong expectedSessionId)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    sessionId != expectedSessionId ||
                    !lootResolvedCaptured ||
                    lootResolved.RoomId != expectedRoomId ||
                    lootResolved.DropId == 0U ||
                    lootResolved.ItemId == 0U ||
                    lootResolved.Quantity == 0)
                {
                    return false;
                }

                lastError = string.Empty;
                return true;
            }
        }

        private bool TryReturnCapturedLootRejected(
            uint expectedRoomId,
            ulong expectedSessionId)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    sessionId != expectedSessionId ||
                    !lootRejectedCaptured ||
                    lootRejected.RoomId != expectedRoomId ||
                    lootRejected.DropId == 0U)
                {
                    return false;
                }

                lastError = string.Empty;
                return true;
            }
        }

        private bool TryReturnCapturedInventorySnapshot(
            ulong expectedSessionId,
            PlayerLootResolved expectedLootResolved)
        {
            lock (syncRoot)
            {
                if (status != PlayerNetworkSessionStatus.Connected ||
                    sessionId != expectedSessionId ||
                    !inventorySnapshotCaptured ||
                    inventorySnapshot.SessionId != expectedSessionId ||
                    expectedLootResolved.WinnerSessionId != expectedSessionId ||
                    expectedLootResolved.DropId == 0U ||
                    !InventoryContainsResolvedItem(
                        inventorySnapshot,
                        expectedLootResolved.ItemId,
                        expectedLootResolved.Quantity))
                {
                    return false;
                }

                lastError = string.Empty;
                return true;
            }
        }

        private bool CapturedDropMatches(uint dropId, uint itemId, ushort quantity)
        {
            for (int index = 0; index < dropListSnapshotV2.Count; ++index)
            {
                PlayerDropEntryV2 drop = dropListSnapshotV2.DropAt(index);
                if (drop.DropId == dropId)
                {
                    return drop.ItemId == itemId && drop.Quantity == quantity;
                }
            }

            return false;
        }

        private bool CanApplyLootResolvedFromCurrentDropState(
            uint dropId,
            uint itemId,
            ushort quantity)
        {
            if (CapturedDropMatches(dropId, itemId, quantity))
            {
                return true;
            }

            return dropListSnapshotV2Captured && dropListSnapshotV2.Count == 0;
        }

        private bool CapturedDropExists(uint dropId)
        {
            for (int index = 0; index < dropListSnapshotV2.Count; ++index)
            {
                if (dropListSnapshotV2.DropAt(index).DropId == dropId)
                {
                    return true;
                }
            }

            return false;
        }

        private static bool InventoryContainsResolvedItem(
            PlayerInventorySnapshot snapshot,
            uint itemId,
            ushort quantity)
        {
            uint totalQuantity = 0U;
            for (int index = 0; index < snapshot.Count; ++index)
            {
                PlayerInventoryEntry entry = snapshot.EntryAt(index);
                if (entry.ItemId == itemId)
                {
                    totalQuantity += entry.Quantity;
                }
            }

            return totalQuantity >= quantity;
        }

        private static bool ContainsSessionId(ulong[] sessionIds, ulong candidateSessionId)
        {
            if (sessionIds == null || candidateSessionId == 0UL)
            {
                return false;
            }

            for (int index = 0; index < sessionIds.Length; ++index)
            {
                if (sessionIds[index] == candidateSessionId)
                {
                    return true;
                }
            }

            return false;
        }

        private static bool RankingContainsSessionId(
            PlayerBattleFinalRanking ranking,
            ulong candidateSessionId)
        {
            if (candidateSessionId == 0UL)
            {
                return false;
            }

            for (int index = 0; index < ranking.Count; ++index)
            {
                if (ranking.RowAt(index).SessionId == candidateSessionId)
                {
                    return true;
                }
            }

            return false;
        }

        private static ulong[] CopyUlongs(ulong[] values)
        {
            if (values == null || values.Length == 0)
            {
                return Array.Empty<ulong>();
            }

            ulong[] copy = new ulong[values.Length];
            Array.Copy(values, copy, values.Length);
            return copy;
        }

        public void Dispose()
        {
            Disconnect();
            connector.Dispose();
            rudpSender.Dispose();
        }
    }
}
