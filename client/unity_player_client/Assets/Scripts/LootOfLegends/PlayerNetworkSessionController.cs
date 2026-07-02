using System.Threading.Tasks;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public sealed class PlayerNetworkSessionController :
        MonoBehaviour,
        IPlayerMoveIntentSender,
        IPlayerSpaceLootIntentSender,
        IPlayerManualRoomCommandSession,
        IPlayerManualBattleCommandSession,
        IPlayerManualCombatCommandSession,
        IPlayerManualLootCommandSession
    {
        public const float HeartbeatIntervalSeconds = 3.0f;

        [SerializeField]
        private string host = PlayerServerEndpoint.DefaultHost;

        [SerializeField]
        private int tcpPort = PlayerServerEndpoint.DefaultTcpPort;

        [SerializeField]
        private int rudpPort = PlayerServerEndpoint.DefaultRudpPort;

        [SerializeField]
        private bool connectOnStart;

        private PlayerNetworkSession session;
        private bool incomingTcpDrainInFlight;
        private bool heartbeatInFlight;
        private float nextHeartbeatAtSeconds;

        public PlayerServerEndpoint Endpoint
        {
            get
            {
                if (PlayerServerEndpoint.TryCreate(host, tcpPort, rudpPort, out PlayerServerEndpoint endpoint))
                {
                    return endpoint;
                }

                return default;
            }
        }

        public PlayerNetworkSessionStatus Status =>
            session != null ? session.Status : PlayerNetworkSessionStatus.Disconnected;

        public string LastError => session != null ? session.LastError : string.Empty;

        public ulong SessionId => session != null ? session.SessionId : 0UL;

        public int ClientSessionCount => session != null ? session.ClientSessionCount : 0;

        public bool SelfListedInClientListSnapshot =>
            session != null && session.SelfListedInClientListSnapshot;

        public ulong[] ClientSessionIds =>
            session != null ? session.ClientSessionIds : new ulong[0];

        public uint CurrentRoomId => session != null ? session.CurrentRoomId : 0U;

        public ushort CurrentRoomPlayerCount =>
            session != null ? session.CurrentRoomPlayerCount : (ushort)0;

        public ushort CurrentRoomReadyPlayerCount =>
            session != null ? session.CurrentRoomReadyPlayerCount : (ushort)0;

        public byte CurrentRoomMaxPlayers =>
            session != null ? session.CurrentRoomMaxPlayers : (byte)0;

        public bool BattleStarted => session != null && session.BattleStarted;

        public uint BattleStartRoomId => session != null ? session.BattleStartRoomId : 0U;

        public ulong BattleStartPlayerASessionId =>
            session != null ? session.BattleStartPlayerASessionId : 0UL;

        public ulong BattleStartPlayerBSessionId =>
            session != null ? session.BattleStartPlayerBSessionId : 0UL;

        public bool BattleLoadEntryCaptured =>
            session != null && session.BattleLoadEntryCaptured;

        public uint BattleLoadEntryRoomId =>
            session != null ? session.BattleLoadEntryRoomId : 0U;

        public ulong BattleInstanceId =>
            session != null ? session.BattleInstanceId : 0UL;

        public ulong[] BattleLoadPlayerSessionIds =>
            session != null ? session.BattleLoadPlayerSessionIds : new ulong[0];

        public bool ArenaLoadCompleteSent =>
            session != null && session.ArenaLoadCompleteSent;

        public bool ArenaGameplayStarted =>
            session != null && session.ArenaGameplayStarted;

        public uint ArenaGameplayStartRoomId =>
            session != null ? session.ArenaGameplayStartRoomId : 0U;

        public ulong ArenaGameplayStartBattleInstanceId =>
            session != null ? session.ArenaGameplayStartBattleInstanceId : 0UL;

        public bool MonsterSpawned => session != null && session.MonsterSpawned;

        public uint MonsterSpawnRoomId => session != null ? session.MonsterSpawnRoomId : 0U;

        public uint MonsterId => session != null ? session.MonsterId : 0U;

        public uint MonsterTypeId => session != null ? session.MonsterTypeId : 0U;

        public ushort MonsterMaxHp => session != null ? session.MonsterMaxHp : (ushort)0;

        public bool MonsterHealthSnapshotCaptured =>
            session != null && session.MonsterHealthSnapshotCaptured;

        public uint MonsterHealthRoomId =>
            session != null ? session.MonsterHealthRoomId : 0U;

        public uint MonsterHealthMonsterId =>
            session != null ? session.MonsterHealthMonsterId : 0U;

        public ushort MonsterCurrentHp =>
            session != null ? session.MonsterCurrentHp : (ushort)0;

        public ushort MonsterHealthMaxHp =>
            session != null ? session.MonsterHealthMaxHp : (ushort)0;

        public bool MonsterDeathCaptured =>
            session != null && session.MonsterDeathCaptured;

        public uint MonsterDeathRoomId =>
            session != null ? session.MonsterDeathRoomId : 0U;

        public uint MonsterDeathMonsterId =>
            session != null ? session.MonsterDeathMonsterId : 0U;

        public bool DropListSnapshotV2Captured =>
            session != null && session.DropListSnapshotV2Captured;

        public uint DropListSnapshotV2RoomId =>
            session != null ? session.DropListSnapshotV2RoomId : 0U;

        public uint DropListSnapshotV2ScatterSeed =>
            session != null ? session.DropListSnapshotV2ScatterSeed : 0U;

        public int DropListSnapshotV2DropCount =>
            session != null ? session.DropListSnapshotV2DropCount : 0;

        public PlayerDropEntryV2[] DropListSnapshotV2Drops =>
            session != null ? session.DropListSnapshotV2Drops : new PlayerDropEntryV2[0];

        public int RoomListCount => session != null ? session.RoomListCount : 0;

        public PlayerRoomListEntry[] RoomListEntries =>
            session != null ? session.RoomListEntries : new PlayerRoomListEntry[0];

        public bool RoomDetailCaptured =>
            session != null && session.RoomDetailCaptured;

        public PlayerRoomStatus CurrentRoomStatus =>
            session != null ? session.CurrentRoomStatus : PlayerRoomStatus.Open;

        public string RoomTitle =>
            session != null ? session.RoomTitle : string.Empty;

        public PlayerRoomMemberEntry[] RoomMembers =>
            session != null ? session.RoomMembers : new PlayerRoomMemberEntry[0];

        public ushort SelfRoomActionMask =>
            session != null ? session.SelfRoomActionMask : (ushort)0;

        public PlayerRoomTargetActionEntry[] RoomTargetActions =>
            session != null ? session.RoomTargetActions : new PlayerRoomTargetActionEntry[0];

        public bool RudpHelloSent => session != null && session.RudpHelloSent;

        public ulong RudpHelloSessionId => session != null ? session.RudpHelloSessionId : 0UL;

        public uint RudpHelloSequence => session != null ? session.RudpHelloSequence : 0U;

        public string RudpHelloLocalEndpoint =>
            session != null ? session.RudpHelloLocalEndpoint : string.Empty;

        public bool RudpAttackSent => session != null && session.RudpAttackSent;

        public ulong RudpAttackSessionId =>
            session != null ? session.RudpAttackSessionId : 0UL;

        public uint RudpAttackSequence =>
            session != null ? session.RudpAttackSequence : 0U;

        public uint RudpAttackCommandSequence =>
            session != null ? session.RudpAttackCommandSequence : 0U;

        public uint RudpAttackTargetHintMonsterId =>
            session != null ? session.RudpAttackTargetHintMonsterId : 0U;

        public string RudpAttackLocalEndpoint =>
            session != null ? session.RudpAttackLocalEndpoint : string.Empty;

        public bool RudpSpaceLootSent =>
            session != null && session.RudpSpaceLootSent;

        public ulong RudpSpaceLootSessionId =>
            session != null ? session.RudpSpaceLootSessionId : 0UL;

        public uint RudpSpaceLootSequence =>
            session != null ? session.RudpSpaceLootSequence : 0U;

        public uint RudpSpaceLootCommandSequence =>
            session != null ? session.RudpSpaceLootCommandSequence : 0U;

        public string RudpSpaceLootLocalEndpoint =>
            session != null ? session.RudpSpaceLootLocalEndpoint : string.Empty;

        public bool RudpMoveSent =>
            session != null && session.RudpMoveSent;

        public ulong RudpMoveSessionId =>
            session != null ? session.RudpMoveSessionId : 0UL;

        public uint RudpMoveSequence =>
            session != null ? session.RudpMoveSequence : 0U;

        public uint RudpMoveCommandSequence =>
            session != null ? session.RudpMoveCommandSequence : 0U;

        public string RudpMoveLocalEndpoint =>
            session != null ? session.RudpMoveLocalEndpoint : string.Empty;

        public bool StateSnapshotCaptured =>
            session != null && session.StateSnapshotCaptured;

        public PlayerRudpStateSnapshot StateSnapshot =>
            session != null ? session.StateSnapshot : default;

        public bool LootResolvedCaptured =>
            session != null && session.LootResolvedCaptured;

        public PlayerLootResolved LootResolved =>
            session != null ? session.LootResolved : default;

        public bool LootRejectedCaptured =>
            session != null && session.LootRejectedCaptured;

        public PlayerLootRejected LootRejected =>
            session != null ? session.LootRejected : default;

        public bool InventorySnapshotCaptured =>
            session != null && session.InventorySnapshotCaptured;

        public PlayerInventorySnapshot InventorySnapshot =>
            session != null ? session.InventorySnapshot : default;

        public PlayerInventoryEntry[] InventorySnapshotEntries =>
            session != null ? session.InventorySnapshotEntries : new PlayerInventoryEntry[0];

        public bool BattleFinalRankingCaptured =>
            session != null && session.BattleFinalRankingCaptured;

        public PlayerBattleFinalRanking BattleFinalRanking =>
            session != null ? session.BattleFinalRanking : default;

        public bool LobbyReturnCaptured =>
            session != null && session.LobbyReturnCaptured;

        public uint LobbyReturnPreviousRoomId =>
            session != null ? session.LobbyReturnPreviousRoomId : 0U;

        public PlayerLobbyReturnReason LobbyReturnReason =>
            session != null ? session.LobbyReturnReason : PlayerLobbyReturnReason.None;

        public PlayerNetworkSession Session
        {
            get
            {
                EnsureSession();
                return session;
            }
        }

        private void Awake()
        {
            EnsureSession();
        }

        private void Start()
        {
            if ((connectOnStart || HasStoredAdmission()) &&
                Status == PlayerNetworkSessionStatus.Disconnected)
            {
                _ = ConnectAsync();
            }
        }

        private void Update()
        {
            if (session == null ||
                session.Status != PlayerNetworkSessionStatus.Connected)
            {
                return;
            }

            session.DrainIncomingRudpStateSnapshot();

            if (!incomingTcpDrainInFlight)
            {
                incomingTcpDrainInFlight = true;
                _ = DrainIncomingTcpStateForFrameAsync();
            }

            TrySendHeartbeatForFrame();
        }

        private async Task DrainIncomingTcpStateForFrameAsync()
        {
            try
            {
                await session.DrainIncomingTcpStateAsync();
            }
            finally
            {
                incomingTcpDrainInFlight = false;
            }
        }

        private void TrySendHeartbeatForFrame()
        {
            if (heartbeatInFlight || Time.unscaledTime < nextHeartbeatAtSeconds)
            {
                return;
            }

            heartbeatInFlight = true;
            nextHeartbeatAtSeconds = Time.unscaledTime + HeartbeatIntervalSeconds;
            _ = SendHeartbeatForFrameAsync(session);
        }

        private async Task SendHeartbeatForFrameAsync(PlayerNetworkSession heartbeatSession)
        {
            try
            {
                if (heartbeatSession != null)
                {
                    await heartbeatSession.SendHeartbeatAsync();
                }
            }
            finally
            {
                heartbeatInFlight = false;
            }
        }

        public Task ConnectAsync()
        {
            EnsureSession();
            GameSessionRoot root = GameSessionRoot.Instance;
            if (root != null && root.HasAdmission)
            {
                return session.ConnectAsync(root.GameServerEndpoint, root.GameSessionToken);
            }

            return session.ConnectAsync(Endpoint);
        }

        private static bool HasStoredAdmission()
        {
            GameSessionRoot root = GameSessionRoot.Instance;
            return root != null && root.HasAdmission;
        }

        public Task<bool> RequestCreateRoomAsync()
        {
            EnsureSession();
            return session.RequestCreateRoomAsync();
        }

        public Task<bool> RequestCreateRoomAsync(string roomTitle, int maxPlayers)
        {
            EnsureSession();
            return session.RequestCreateRoomAsync(roomTitle, maxPlayers);
        }

        public Task<bool> CaptureRoomListSnapshotAsync()
        {
            EnsureSession();
            return session.CaptureRoomListSnapshotAsync();
        }

        public Task<bool> RequestJoinRoomAsync(uint roomId)
        {
            EnsureSession();
            return session.RequestJoinRoomAsync(roomId);
        }

        public Task<bool> RequestReadyRoomAsync()
        {
            EnsureSession();
            return session.RequestReadyRoomAsync();
        }

        public Task<bool> RequestUnreadyRoomAsync()
        {
            EnsureSession();
            return session.RequestUnreadyRoomAsync();
        }

        public Task<bool> RequestHostStartBattleAsync()
        {
            EnsureSession();
            return session.RequestHostStartBattleAsync();
        }

        public Task<bool> RequestHostKickAsync(uint targetSessionId)
        {
            EnsureSession();
            return session.RequestHostKickAsync(targetSessionId);
        }

        public Task<bool> RequestLeaveRoomAsync()
        {
            EnsureSession();
            return session.RequestLeaveRoomAsync();
        }

        public Task<bool> CaptureBattleStartAsync()
        {
            EnsureSession();
            return session.CaptureBattleStartAsync();
        }

        public Task<bool> CaptureBattleLoadEntryAsync()
        {
            EnsureSession();
            return session.CaptureBattleLoadEntryAsync();
        }

        public Task<bool> SendArenaLoadCompleteAsync()
        {
            EnsureSession();
            return session.SendArenaLoadCompleteAsync();
        }

        public Task<bool> CaptureArenaGameplayStartAsync()
        {
            EnsureSession();
            return session.CaptureArenaGameplayStartAsync();
        }

        public Task<bool> CaptureMonsterSpawnAsync()
        {
            EnsureSession();
            return session.CaptureMonsterSpawnAsync();
        }

        public Task<bool> CaptureMonsterHealthSnapshotAsync()
        {
            EnsureSession();
            return session.CaptureMonsterHealthSnapshotAsync();
        }

        public Task<bool> CaptureMonsterDeathAsync()
        {
            EnsureSession();
            return session.CaptureMonsterDeathAsync();
        }

        public Task<bool> CaptureDropListSnapshotV2Async()
        {
            EnsureSession();
            return session.CaptureDropListSnapshotV2Async();
        }

        public Task<bool> SendAttackIntentAsync()
        {
            EnsureSession();
            return session.SendAttackIntentAsync();
        }

        public Task<bool> SendMoveIntentAsync(PlayerInputIntent intent)
        {
            EnsureSession();
            return session.SendMoveIntentAsync(intent);
        }

        public Task<bool> SendSpaceLootIntentAsync()
        {
            EnsureSession();
            return session.SendSpaceLootIntentAsync();
        }

        public Task<bool> CaptureLootResolvedAsync()
        {
            EnsureSession();
            return session.CaptureLootResolvedAsync();
        }

        public Task<bool> CaptureLootRejectedAsync()
        {
            EnsureSession();
            return session.CaptureLootRejectedAsync();
        }

        public Task<bool> CaptureInventorySnapshotAsync()
        {
            EnsureSession();
            return session.CaptureInventorySnapshotAsync();
        }

        public Task<bool> CaptureBattleFinalRankingAsync()
        {
            EnsureSession();
            return session.CaptureBattleFinalRankingAsync();
        }

        public Task<bool> CaptureLobbyReturnVisibilityAsync()
        {
            EnsureSession();
            return session.CaptureLobbyReturnVisibilityAsync();
        }

        public Task<bool> DrainIncomingTcpStateAsync()
        {
            EnsureSession();
            return session.DrainIncomingTcpStateAsync();
        }

        public bool DrainIncomingRudpStateSnapshot()
        {
            EnsureSession();
            return session.DrainIncomingRudpStateSnapshot();
        }

        public void Disconnect()
        {
            if (session != null)
            {
                session.Disconnect();
            }
        }

        private void OnDestroy()
        {
            if (session != null)
            {
                session.Dispose();
                session = null;
            }
        }

        private void EnsureSession()
        {
            if (session == null)
            {
                session = new PlayerNetworkSession(new PlayerTcpNetworkConnector());
            }
        }
    }
}
