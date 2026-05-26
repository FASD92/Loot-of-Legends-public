using System.Collections.Generic;
using System.Globalization;
using System.Text;
using UnityEngine;

namespace LootOfLegends.ThinClient
{
    internal sealed class ThinClientDebugUi : MonoBehaviour
    {
        private const int MaxLogLines = 120;
        private const short MoveIntentScale = 1000;
        private const float SnapshotRenderScale = 0.1f;
        private const float SnapshotMarkerSize = 0.28f;
        private const float CenterItemMarkerSize = 0.36f;
        private const float SnapshotMarkerZ = 0f;
        private const float CenterItemMarkerZ = -0.02f;

        private readonly List<string> logLines = new List<string>(MaxLogLines);
        private readonly Dictionary<ulong, GameObject> snapshotMarkers = new Dictionary<ulong, GameObject>();
        private TcpDebugSession clientA;
        private TcpDebugSession clientB;
        private RudpHelloClient rudpA;
        private RudpHelloClient rudpB;
        private string host = "127.0.0.1";
        private string portText = "40000";
        private string manualRoomIdText = string.Empty;
        private Vector2 logScroll;
        private GameObject snapshotRenderRoot;
        private GameObject centerItemMarker;
        private Sprite snapshotMarkerSprite;
        private Sprite centerItemMarkerSprite;
        private bool hasRenderedSnapshot;
        private bool hasRenderedCenterItem;
        private string lastRenderSource = "-";
        private uint lastRenderedRoomId;
        private uint lastRenderedTick;
        private uint lastRenderedCenterDropId;
        private float lastRenderedRealtime;
        private int lastRenderedMarkerCount;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Bootstrap()
        {
            if (FindFirstObjectByType<ThinClientDebugUi>() != null)
            {
                return;
            }

            GameObject hostObject = new GameObject("LootOfLegendsThinClientDebugUi");
            DontDestroyOnLoad(hostObject);
            hostObject.AddComponent<ThinClientDebugUi>();
        }

        private void Awake()
        {
            clientA = new TcpDebugSession("A");
            clientB = new TcpDebugSession("B");
            rudpA = new RudpHelloClient(101);
            rudpB = new RudpHelloClient(102);
            AppendLog("Unity TCP debug client ready");
        }

        private void Update()
        {
            DrainSessionLogs(clientA);
            DrainSessionLogs(clientB);
            DrainRudpLogs(rudpA);
            DrainRudpLogs(rudpB);
            TickRudpHeartbeat(clientA, rudpA);
            TickRudpHeartbeat(clientB, rudpB);
            UpdateSnapshotMarkers();
            UpdateCenterItemMarker();
        }

        private void OnDestroy()
        {
            if (clientA != null)
            {
                clientA.Dispose();
                clientA = null;
            }
            if (clientB != null)
            {
                clientB.Dispose();
                clientB = null;
            }
            if (rudpA != null)
            {
                rudpA.Dispose();
                rudpA = null;
            }
            if (rudpB != null)
            {
                rudpB.Dispose();
                rudpB = null;
            }
            ClearSnapshotMarkers();
            ClearCenterItemMarker();
            if (snapshotMarkerSprite != null)
            {
                Destroy(snapshotMarkerSprite);
                snapshotMarkerSprite = null;
            }
            if (centerItemMarkerSprite != null)
            {
                Destroy(centerItemMarkerSprite);
                centerItemMarkerSprite = null;
            }
            if (snapshotRenderRoot != null)
            {
                Destroy(snapshotRenderRoot);
                snapshotRenderRoot = null;
            }
        }

        private void OnGUI()
        {
            float panelWidth = Mathf.Min(720f, Mathf.Max(320f, Screen.width - 20f));
            float panelHeight = Mathf.Min(620f, Mathf.Max(360f, Screen.height - 20f));

            GUILayout.BeginArea(new Rect(10f, 10f, panelWidth, panelHeight), GUI.skin.box);
            GUILayout.Label("Loot of Legends TCP Debug");

            GUILayout.BeginHorizontal();
            GUILayout.Label("Host", GUILayout.Width(42f));
            host = GUILayout.TextField(host, GUILayout.Width(180f));
            GUILayout.Label("Port", GUILayout.Width(36f));
            portText = GUILayout.TextField(portText, GUILayout.Width(72f));
            if (GUILayout.Button("Connect A", GUILayout.Width(92f)))
            {
                ConnectSession(clientA);
            }
            if (GUILayout.Button("Connect B", GUILayout.Width(92f)))
            {
                ConnectSession(clientB);
            }
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            if (GUILayout.Button("A CreateRoom", GUILayout.Width(120f)))
            {
                clientA.SendCreateRoom();
            }
            if (GUILayout.Button("B Join Latest", GUILayout.Width(120f)))
            {
                if (TryResolveRoomId(out uint roomId))
                {
                    clientB.SendJoinRoom(roomId);
                }
                else
                {
                    AppendLog("No confirmed roomId for B Join");
                }
            }
            if (GUILayout.Button("A Ready", GUILayout.Width(90f)))
            {
                clientA.SendReady();
            }
            if (GUILayout.Button("B Ready", GUILayout.Width(90f)))
            {
                clientB.SendReady();
            }
            if (GUILayout.Button("Disconnect", GUILayout.Width(100f)))
            {
                clientA.Disconnect();
                clientB.Disconnect();
                rudpA.ResetEndpoint();
                rudpB.ResetEndpoint();
                AppendLog("Disconnected A/B");
            }
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            if (GUILayout.Button("RUDP Hello A", GUILayout.Width(120f)))
            {
                SendRudpHello(clientA, rudpA);
            }
            if (GUILayout.Button("RUDP Hello B", GUILayout.Width(120f)))
            {
                SendRudpHello(clientB, rudpB);
            }
            if (GUILayout.Button("RUDP Ready A", GUILayout.Width(120f)))
            {
                SendRudpReady(clientA, rudpA);
            }
            if (GUILayout.Button("RUDP Ready B", GUILayout.Width(120f)))
            {
                SendRudpReady(clientB, rudpB);
            }
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Move A", GUILayout.Width(54f));
            if (GUILayout.Button("+X", GUILayout.Width(42f)))
            {
                SendRudpMove(clientA, rudpA, MoveIntentScale, 0);
            }
            if (GUILayout.Button("-X", GUILayout.Width(42f)))
            {
                SendRudpMove(clientA, rudpA, -MoveIntentScale, 0);
            }
            if (GUILayout.Button("+Y", GUILayout.Width(42f)))
            {
                SendRudpMove(clientA, rudpA, 0, MoveIntentScale);
            }
            if (GUILayout.Button("-Y", GUILayout.Width(42f)))
            {
                SendRudpMove(clientA, rudpA, 0, -MoveIntentScale);
            }
            if (GUILayout.Button("Stop", GUILayout.Width(56f)))
            {
                SendRudpMove(clientA, rudpA, 0, 0);
            }
            GUILayout.Space(16f);
            GUILayout.Label("Move B", GUILayout.Width(54f));
            if (GUILayout.Button("+X", GUILayout.Width(42f)))
            {
                SendRudpMove(clientB, rudpB, MoveIntentScale, 0);
            }
            if (GUILayout.Button("-X", GUILayout.Width(42f)))
            {
                SendRudpMove(clientB, rudpB, -MoveIntentScale, 0);
            }
            if (GUILayout.Button("+Y", GUILayout.Width(42f)))
            {
                SendRudpMove(clientB, rudpB, 0, MoveIntentScale);
            }
            if (GUILayout.Button("-Y", GUILayout.Width(42f)))
            {
                SendRudpMove(clientB, rudpB, 0, -MoveIntentScale);
            }
            if (GUILayout.Button("Stop", GUILayout.Width(56f)))
            {
                SendRudpMove(clientB, rudpB, 0, 0);
            }
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Item/Loot", GUILayout.Width(70f));
            if (GUILayout.Button("Drop Center Item", GUILayout.Width(132f)))
            {
                RequestDropCenterItemForSmoke();
            }
            if (GUILayout.Button("Place A/B Around", GUILayout.Width(132f)))
            {
                RequestPlacePlayersAroundItemForSmoke();
            }
            if (GUILayout.Button("Loot A/B Simul", GUILayout.Width(120f)))
            {
                RequestLootABSimulForSmoke();
            }
            GUILayout.Label("drop/place/loot sends enabled", GUILayout.Width(220f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Drop Source", GUILayout.Width(86f));
            GUILayout.Label(DropSourceSummary(), GUILayout.Width(600f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Loot Result", GUILayout.Width(86f));
            GUILayout.Label(LootResultSummary(), GUILayout.Width(600f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Winner", GUILayout.Width(86f));
            GUILayout.Label(WinnerResultSummary(), GUILayout.Width(600f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Loser", GUILayout.Width(86f));
            GUILayout.Label(LoserResultSummary(), GUILayout.Width(600f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Inventory", GUILayout.Width(86f));
            GUILayout.Label(InventoryResultSummary(), GUILayout.Width(600f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Manual roomId", GUILayout.Width(104f));
            manualRoomIdText = GUILayout.TextField(manualRoomIdText, GUILayout.Width(96f));
            GUILayout.Label(SessionSummary(clientA), GUILayout.Width(220f));
            GUILayout.Label(SessionSummary(clientB), GUILayout.Width(220f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label("Snapshot A", GUILayout.Width(84f));
            GUILayout.Label(SnapshotSummary(clientA, rudpA), GUILayout.Width(260f));
            GUILayout.Space(12f);
            GUILayout.Label("Snapshot B", GUILayout.Width(84f));
            GUILayout.Label(SnapshotSummary(clientB, rudpB), GUILayout.Width(260f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label(SnapshotStatsSummary("A", rudpA), GUILayout.Width(220f));
            GUILayout.Label(SnapshotStatsSummary("B", rudpB), GUILayout.Width(220f));
            GUILayout.Label(RenderStatsSummary(), GUILayout.Width(260f));
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUILayout.Label(HeartbeatStatsSummary("A", rudpA), GUILayout.Width(220f));
            GUILayout.Label(HeartbeatStatsSummary("B", rudpB), GUILayout.Width(220f));
            GUILayout.EndHorizontal();

            GUILayout.Space(6f);
            logScroll = GUILayout.BeginScrollView(logScroll, GUILayout.ExpandHeight(true));
            for (int i = 0; i < logLines.Count; ++i)
            {
                GUILayout.Label(logLines[i]);
            }
            GUILayout.EndScrollView();
            GUILayout.EndArea();
        }

        private void ConnectSession(TcpDebugSession session)
        {
            if (!TryResolvePort(true, out ushort port))
            {
                return;
            }

            session.Connect(host, port);
        }

        private void SendRudpHello(TcpDebugSession session, RudpHelloClient rudpClient)
        {
            if (!TryResolvePort(true, out ushort port))
            {
                return;
            }

            ulong sessionId = session.SessionId;
            if (sessionId == 0)
            {
                AppendLog(session.Alias + " cannot send RUDP Hello before TCP Welcome");
                return;
            }

            rudpClient.SendHello(host, port, sessionId, out string message);
            AppendLog(session.Alias + " " + message);
        }

        private void SendRudpReady(TcpDebugSession session, RudpHelloClient rudpClient)
        {
            if (!TryResolvePort(true, out ushort port))
            {
                return;
            }

            ulong sessionId = session.SessionId;
            if (sessionId == 0)
            {
                AppendLog(session.Alias + " cannot send RUDP Ready before TCP Welcome");
                return;
            }

            rudpClient.SendReadyInputCommand(host, port, sessionId, out string message);
            AppendLog(session.Alias + " " + message);
        }

        private void SendRudpMove(
            TcpDebugSession session,
            RudpHelloClient rudpClient,
            short dirX,
            short dirY)
        {
            if (!TryResolvePort(true, out ushort port))
            {
                return;
            }

            ulong sessionId = session.SessionId;
            if (sessionId == 0)
            {
                AppendLog(session.Alias + " cannot send RUDP Move before TCP Welcome");
                return;
            }

            rudpClient.SendMoveInputCommand(host, port, sessionId, dirX, dirY, out string message);
            AppendLog(session.Alias + " " + message);
        }

        private void RequestDropCenterItemForSmoke()
        {
            if (!TryResolveSmokeRoom("Drop Center Item"))
            {
                return;
            }

            clientA.SendSmokeCreateCenterDropRequest();
            AppendLog("Drop Center Item sent: SmokeCreateCenterDropRequest from A; waiting for server-origin DropListSnapshot");
        }

        private void RequestPlacePlayersAroundItemForSmoke()
        {
            if (!TryResolveSmokeRoom("Place A/B Around"))
            {
                return;
            }

            if (!TrySelectObservedDropForSmoke(out ObservedDropSourceView dropSource, out string reason))
            {
                AppendLog("Place A/B Around blocked: " + reason + "; no packet sent");
                return;
            }

            clientA.SendSmokePlacePlayersAroundCenterDropRequest();
            AppendLog("Place A/B Around sent: SmokePlacePlayersAroundCenterDropRequest from A for server-origin dropId=" +
                      dropSource.DropId +
                      "; waiting for StateSnapshot placement");
        }

        private void RequestLootABSimulForSmoke()
        {
            if (!TryResolveSmokeRoom("Loot A/B Simul"))
            {
                return;
            }

            if (!TrySelectObservedDropForSmoke(out ObservedDropSourceView dropSource, out string reason))
            {
                AppendLog("Loot A/B Simul blocked: " + reason + "; no packet sent");
                return;
            }

            clientA.SendClickLootRequest(dropSource.DropId);
            clientB.SendClickLootRequest(dropSource.DropId);
            AppendLog("Loot A/B Simul sent: ClickLootRequest A then B for server-origin dropId=" +
                      dropSource.DropId +
                      " via " + dropSource.Source +
                      "; waiting for LootResolved + LootRejected + InventorySnapshot");
        }

        private bool TryResolveSmokeRoom(string action)
        {
            if (!clientA.IsConnected || !clientB.IsConnected)
            {
                AppendLog(action + " blocked: connect A/B and keep both TCP sessions open first");
                return false;
            }

            if (clientA.SessionId == 0 || clientB.SessionId == 0)
            {
                AppendLog(action + " blocked: connect A/B and wait for TCP Welcome first");
                return false;
            }

            uint roomA = clientA.RoomId;
            uint roomB = clientB.RoomId;
            if (roomA == 0 || roomB == 0 || roomA != roomB)
            {
                AppendLog(action + " blocked: A/B must share a server-confirmed room first");
                return false;
            }

            return true;
        }

        private void TickRudpHeartbeat(TcpDebugSession session, RudpHelloClient rudpClient)
        {
            if (!TryResolvePort(false, out ushort port))
            {
                return;
            }

            ulong sessionId = session.SessionId;
            if (sessionId == 0)
            {
                return;
            }

            if (rudpClient.TickHelloHeartbeat(host, port, sessionId, out string message) &&
                !string.IsNullOrEmpty(message))
            {
                AppendLog(session.Alias + " " + message);
            }
        }

        private bool TryResolvePort(bool logError, out ushort port)
        {
            if (ushort.TryParse(portText, out port) && port != 0)
            {
                return true;
            }

            port = 0;
            if (logError)
            {
                AppendLog("Invalid port: " + portText);
            }
            return false;
        }

        private bool TryResolveRoomId(out uint roomId)
        {
            roomId = clientA.RoomId;
            if (roomId != 0)
            {
                return true;
            }

            return uint.TryParse(manualRoomIdText, out roomId) && roomId != 0;
        }

        private string SessionSummary(TcpDebugSession session)
        {
            string connection = session.IsConnected ? "connected" : "closed";
            string sessionText = session.SessionId == 0 ? "-" : session.SessionId.ToString();
            string roomText = session.RoomId == 0 ? "-" : session.RoomId.ToString();
            return session.Alias + " " + connection + " session=" + sessionText + " room=" + roomText;
        }

        private string DropSourceSummary()
        {
            if (TrySelectObservedDropForSmoke(out ObservedDropSourceView dropSource, out string reason))
            {
                return "server-origin dropId=" + dropSource.DropId +
                       " item=" + dropSource.ItemId +
                       " qty=" + dropSource.Quantity +
                       " via " + dropSource.Source;
            }

            return reason;
        }

        private string LootResultSummary()
        {
            bool hasDrop = TrySelectObservedDropForSmoke(
                out ObservedDropSourceView dropSource,
                out string dropReason);
            bool hasResolved = TrySelectLootResolved(hasDrop, dropSource, out LootResolvedView resolved);
            bool hasRejected = TrySelectLootRejected(hasDrop, dropSource, out LootRejectedView rejected);
            bool hasInventory = TrySelectInventorySnapshot(
                hasResolved,
                resolved,
                out InventorySnapshotView inventory);

            bool fullPass =
                clientA.IsConnected &&
                clientB.IsConnected &&
                hasDrop &&
                hasResolved &&
                hasRejected &&
                hasInventory &&
                resolved.RoomId == dropSource.RoomId &&
                resolved.DropId == dropSource.DropId &&
                rejected.RoomId == dropSource.RoomId &&
                rejected.DropId == dropSource.DropId &&
                rejected.Reason == TcpLootRejectReason.AlreadyClaimed &&
                rejected.SessionId != resolved.WinnerSessionId &&
                inventory.SessionId == resolved.WinnerSessionId &&
                inventory.ContainsResolvedItem;

            StringBuilder builder = new StringBuilder(220);
            builder.Append(fullPass ? "Full PASS" : "partial");
            builder.Append(" source=").Append(hasDrop ? dropSource.DropId.ToString() : dropReason);
            builder.Append(" resolved=");
            if (hasResolved)
            {
                builder.Append(resolved.Source)
                       .Append(":winner=")
                       .Append(resolved.WinnerSessionId)
                       .Append("/drop=")
                       .Append(resolved.DropId);
            }
            else
            {
                builder.Append("-");
            }

            builder.Append(" reject=");
            if (hasRejected)
            {
                builder.Append(rejected.Source)
                       .Append(":")
                       .Append(rejected.Reason)
                       .Append("/session=")
                       .Append(rejected.SessionId);
            }
            else
            {
                builder.Append("-");
            }

            builder.Append(" inv=");
            if (hasInventory)
            {
                builder.Append(inventory.Source)
                       .Append(":")
                       .Append(inventory.CurrentWeight)
                       .Append("/")
                       .Append(inventory.MaxWeight);
            }
            else
            {
                builder.Append("-");
            }

            return builder.ToString();
        }

        private string SnapshotSummary(TcpDebugSession session, RudpHelloClient rudpClient)
        {
            if (!rudpClient.TryLatestStateSnapshot(out RudpPacketCodec.RudpStateSnapshotPayload snapshot))
            {
                return "snapshot=-";
            }

            StringBuilder builder = new StringBuilder(128);
            builder.Append("room=").Append(snapshot.RoomId)
                   .Append(" tick=").Append(snapshot.ServerTick)
                   .Append(" players=").Append(snapshot.Players.Length);
            for (int i = 0; i < snapshot.Players.Length; ++i)
            {
                RudpPacketCodec.RudpStateSnapshotPlayer player = snapshot.Players[i];
                builder.Append(i == 0 ? " " : " | ");
                if (player.SessionId == session.SessionId)
                {
                    builder.Append("self");
                }
                else
                {
                    builder.Append(player.SessionId);
                }
                builder.Append("=(")
                       .Append(player.WorldX.ToString("0.###", CultureInfo.InvariantCulture))
                       .Append(",")
                       .Append(player.WorldY.ToString("0.###", CultureInfo.InvariantCulture))
                       .Append(")");
            }
            return builder.ToString();
        }

        private string HeartbeatStatsSummary(string alias, RudpHelloClient rudpClient)
        {
            RudpHelloClient.HelloHeartbeatStats stats = rudpClient.HeartbeatStats();
            if (!stats.Active)
            {
                return alias + " hb=-";
            }

            string lastAgeText = stats.LastAgeSeconds < 0.0
                ? "-"
                : Mathf.RoundToInt((float)(stats.LastAgeSeconds * 1000.0)).ToString();
            int nextDueMs = Mathf.RoundToInt((float)(stats.NextDueSeconds * 1000.0));
            return alias + " hb=" + stats.SentCount +
                   " lastMs=" + lastAgeText +
                   " nextMs=" + nextDueMs;
        }

        private string SnapshotStatsSummary(string alias, RudpHelloClient rudpClient)
        {
            RudpHelloClient.StateSnapshotStats stats = rudpClient.SnapshotStats();
            if (!stats.HasSnapshot)
            {
                return alias + " snapshot recv=0 age=-";
            }

            int ageMs = Mathf.RoundToInt((float)(stats.AgeSeconds * 1000.0));
            return alias + " recv=" + stats.ReceivedCount +
                   " tick=" + stats.ServerTick +
                   " ageMs=" + ageMs +
                   " logTick=" + stats.LastLoggedTick;
        }

        private string RenderStatsSummary()
        {
            if (!hasRenderedSnapshot)
            {
                return "render source=- item=" +
                       (hasRenderedCenterItem ? lastRenderedCenterDropId.ToString() : "-");
            }

            int ageMs = Mathf.RoundToInt((Time.realtimeSinceStartup - lastRenderedRealtime) * 1000f);
            return "render source=" + lastRenderSource +
                   " room=" + lastRenderedRoomId +
                   " tick=" + lastRenderedTick +
                   " ageMs=" + ageMs +
                   " markers=" + lastRenderedMarkerCount +
                   " item=" + (hasRenderedCenterItem ? lastRenderedCenterDropId.ToString() : "-");
        }

        private string WinnerResultSummary()
        {
            bool hasDrop = TrySelectObservedDropForSmoke(
                out ObservedDropSourceView dropSource,
                out string dropReason);
            if (!TrySelectLootResolved(hasDrop, dropSource, out LootResolvedView resolved))
            {
                return hasDrop ? "winner=-" : "winner=- source=" + dropReason;
            }

            string matchText = hasDrop &&
                               resolved.RoomId == dropSource.RoomId &&
                               resolved.DropId == dropSource.DropId
                ? "same-drop"
                : "unmatched-drop";
            return resolved.Source +
                   " winner=" + resolved.WinnerSessionId +
                   " room=" + resolved.RoomId +
                   " drop=" + resolved.DropId +
                   " item=" + resolved.ItemId +
                   " qty=" + resolved.Quantity +
                   " " + matchText;
        }

        private string LoserResultSummary()
        {
            bool hasDrop = TrySelectObservedDropForSmoke(
                out ObservedDropSourceView dropSource,
                out string dropReason);
            if (!TrySelectLootRejected(hasDrop, dropSource, out LootRejectedView rejected))
            {
                return hasDrop ? "loser=-" : "loser=- source=" + dropReason;
            }

            string matchText = hasDrop &&
                               rejected.RoomId == dropSource.RoomId &&
                               rejected.DropId == dropSource.DropId
                ? "same-drop"
                : "unmatched-drop";
            string passSurface = rejected.Reason == TcpLootRejectReason.AlreadyClaimed
                ? "AlreadyClaimed PASS-surface"
                : "non-pass reason";
            return rejected.Source +
                   " loserSession=" + rejected.SessionId +
                   " room=" + rejected.RoomId +
                   " drop=" + rejected.DropId +
                   " reason=" + rejected.Reason +
                   " " + matchText +
                   " " + passSurface;
        }

        private string InventoryResultSummary()
        {
            bool hasDrop = TrySelectObservedDropForSmoke(
                out ObservedDropSourceView dropSource,
                out string dropReason);
            if (!TrySelectLootResolved(hasDrop, dropSource, out LootResolvedView resolved))
            {
                return hasDrop
                    ? "winner inventory=- need LootResolved winner"
                    : "winner inventory=- source=" + dropReason;
            }
            if (!TrySelectInventorySnapshot(true, resolved, out InventorySnapshotView inventory))
            {
                return "winner inventory=- session=" + resolved.WinnerSessionId;
            }

            string containsText = inventory.ContainsResolvedItem ? "item-observed" : "item-missing";
            return inventory.Source +
                   " session=" + inventory.SessionId +
                   " weight=" + inventory.CurrentWeight +
                   "/" + inventory.MaxWeight +
                   " " + containsText;
        }

        private bool TrySelectObservedDropForSmoke(out ObservedDropSourceView dropSource, out string reason)
        {
            TcpDropSourceObservation sourceA = clientA.DropSource;
            TcpDropSourceObservation sourceB = clientB.DropSource;
            bool sourceAActive = clientA.IsConnected;
            bool sourceBActive = clientB.IsConnected;
            bool observedA = sourceAActive && sourceA.Status == TcpDropSourceStatus.Observed;
            bool observedB = sourceBActive && sourceB.Status == TcpDropSourceStatus.Observed;

            if (observedA && observedB)
            {
                if (sourceA.RoomId == sourceB.RoomId &&
                    sourceA.DropId == sourceB.DropId &&
                    sourceA.ItemId == sourceB.ItemId &&
                    sourceA.Quantity == sourceB.Quantity)
                {
                    dropSource = MakeObservedDropSource(sourceA, "TCP A/B");
                    reason = string.Empty;
                    return true;
                }

                dropSource = new ObservedDropSourceView();
                reason = "ambiguous server-origin dropId across TCP A/B";
                return false;
            }

            if (observedA)
            {
                dropSource = MakeObservedDropSource(sourceA, "TCP A");
                reason = string.Empty;
                return true;
            }

            if (observedB)
            {
                dropSource = MakeObservedDropSource(sourceB, "TCP B");
                reason = string.Empty;
                return true;
            }

            if ((sourceAActive && sourceA.Status == TcpDropSourceStatus.Ambiguous) ||
                (sourceBActive && sourceB.Status == TcpDropSourceStatus.Ambiguous))
            {
                dropSource = new ObservedDropSourceView();
                reason = "ambiguous DropListSnapshot source";
                return false;
            }

            dropSource = new ObservedDropSourceView();
            reason = "no server-origin observed dropId";
            return false;
        }

        private static ObservedDropSourceView MakeObservedDropSource(
            TcpDropSourceObservation source,
            string label)
        {
            return new ObservedDropSourceView
            {
                Source = label,
                RoomId = source.RoomId,
                DropId = source.DropId,
                ItemId = source.ItemId,
                Quantity = source.Quantity
            };
        }

        private bool TrySelectLootResolved(
            bool hasDrop,
            ObservedDropSourceView dropSource,
            out LootResolvedView resolved)
        {
            if (hasDrop)
            {
                if (TrySelectLootResolvedMatchingDrop(dropSource, out resolved))
                {
                    return true;
                }
            }

            if (TryRudpLootResolved("RUDP A", rudpA, false, dropSource, out resolved) ||
                TryRudpLootResolved("RUDP B", rudpB, false, dropSource, out resolved) ||
                TryTcpLootResolved("TCP A", clientA, false, dropSource, out resolved) ||
                TryTcpLootResolved("TCP B", clientB, false, dropSource, out resolved))
            {
                return true;
            }

            resolved = new LootResolvedView();
            return false;
        }

        private bool TrySelectLootResolvedMatchingDrop(
            ObservedDropSourceView dropSource,
            out LootResolvedView resolved)
        {
            return TryRudpLootResolved("RUDP A", rudpA, true, dropSource, out resolved) ||
                   TryRudpLootResolved("RUDP B", rudpB, true, dropSource, out resolved) ||
                   TryTcpLootResolved("TCP A", clientA, true, dropSource, out resolved) ||
                   TryTcpLootResolved("TCP B", clientB, true, dropSource, out resolved);
        }

        private static bool TryRudpLootResolved(
            string source,
            RudpHelloClient rudpClient,
            bool requireDropMatch,
            ObservedDropSourceView dropSource,
            out LootResolvedView resolved)
        {
            if (!rudpClient.TryLatestLootResolved(out RudpPacketCodec.RudpLootResolvedGameEventPayload payload))
            {
                resolved = new LootResolvedView();
                return false;
            }
            if (requireDropMatch &&
                (payload.RoomId != dropSource.RoomId || payload.DropId != dropSource.DropId))
            {
                resolved = new LootResolvedView();
                return false;
            }

            resolved = new LootResolvedView
            {
                Source = source,
                RoomId = payload.RoomId,
                DropId = payload.DropId,
                WinnerSessionId = payload.WinnerSessionId,
                ItemId = payload.ItemId,
                Quantity = payload.Quantity
            };
            return true;
        }

        private static bool TryTcpLootResolved(
            string source,
            TcpDebugSession session,
            bool requireDropMatch,
            ObservedDropSourceView dropSource,
            out LootResolvedView resolved)
        {
            TcpLootResolvedObservation observation = session.LatestLootResolved;
            if (!observation.HasValue)
            {
                resolved = new LootResolvedView();
                return false;
            }
            if (requireDropMatch &&
                (observation.RoomId != dropSource.RoomId || observation.DropId != dropSource.DropId))
            {
                resolved = new LootResolvedView();
                return false;
            }

            resolved = new LootResolvedView
            {
                Source = source,
                RoomId = observation.RoomId,
                DropId = observation.DropId,
                WinnerSessionId = observation.WinnerSessionId,
                ItemId = observation.ItemId,
                Quantity = observation.Quantity
            };
            return true;
        }

        private bool TrySelectLootRejected(
            bool hasDrop,
            ObservedDropSourceView dropSource,
            out LootRejectedView rejected)
        {
            if (TryTcpLootRejected("TCP A", clientA, hasDrop, dropSource, out rejected) ||
                TryTcpLootRejected("TCP B", clientB, hasDrop, dropSource, out rejected))
            {
                return true;
            }

            rejected = new LootRejectedView();
            return false;
        }

        private static bool TryTcpLootRejected(
            string source,
            TcpDebugSession session,
            bool requireDropMatch,
            ObservedDropSourceView dropSource,
            out LootRejectedView rejected)
        {
            TcpLootRejectedObservation observation = session.LatestLootRejected;
            if (!observation.HasValue)
            {
                rejected = new LootRejectedView();
                return false;
            }
            if (requireDropMatch &&
                (observation.RoomId != dropSource.RoomId || observation.DropId != dropSource.DropId))
            {
                rejected = new LootRejectedView();
                return false;
            }

            rejected = new LootRejectedView
            {
                Source = source,
                SessionId = session.SessionId,
                RoomId = observation.RoomId,
                DropId = observation.DropId,
                Reason = observation.Reason
            };
            return true;
        }

        private bool TrySelectInventorySnapshot(
            bool hasResolved,
            LootResolvedView resolved,
            out InventorySnapshotView inventory)
        {
            if (TryTcpInventorySnapshot("TCP A", clientA, hasResolved, resolved, out inventory) ||
                TryTcpInventorySnapshot("TCP B", clientB, hasResolved, resolved, out inventory))
            {
                return true;
            }

            inventory = new InventorySnapshotView();
            return false;
        }

        private static bool TryTcpInventorySnapshot(
            string source,
            TcpDebugSession session,
            bool requireWinnerSession,
            LootResolvedView resolved,
            out InventorySnapshotView inventory)
        {
            TcpInventorySnapshotObservation observation = session.LatestInventorySnapshot;
            if (!observation.HasValue)
            {
                inventory = new InventorySnapshotView();
                return false;
            }
            if (requireWinnerSession && observation.SessionId != resolved.WinnerSessionId)
            {
                inventory = new InventorySnapshotView();
                return false;
            }

            bool containsResolvedItem = requireWinnerSession &&
                                        observation.ContainsItem(resolved.ItemId, resolved.Quantity);
            inventory = new InventorySnapshotView
            {
                Source = source,
                SessionId = observation.SessionId,
                CurrentWeight = observation.CurrentWeight,
                MaxWeight = observation.MaxWeight,
                ContainsResolvedItem = containsResolvedItem
            };
            return true;
        }

        private struct ObservedDropSourceView
        {
            public string Source;
            public uint RoomId;
            public uint DropId;
            public uint ItemId;
            public ushort Quantity;
        }

        private struct LootResolvedView
        {
            public string Source;
            public uint RoomId;
            public uint DropId;
            public ulong WinnerSessionId;
            public uint ItemId;
            public ushort Quantity;
        }

        private struct LootRejectedView
        {
            public string Source;
            public ulong SessionId;
            public uint RoomId;
            public uint DropId;
            public TcpLootRejectReason Reason;
        }

        private struct InventorySnapshotView
        {
            public string Source;
            public ulong SessionId;
            public ushort CurrentWeight;
            public ushort MaxWeight;
            public bool ContainsResolvedItem;
        }

        private void UpdateSnapshotMarkers()
        {
            if (!TrySelectSnapshotForRender(
                    out RudpPacketCodec.RudpStateSnapshotPayload snapshot,
                    out string renderSource))
            {
                SetSnapshotMarkersActive(false);
                hasRenderedSnapshot = false;
                lastRenderSource = "-";
                lastRenderedMarkerCount = 0;
                return;
            }

            if (hasRenderedSnapshot &&
                lastRenderedRoomId == snapshot.RoomId &&
                lastRenderedTick == snapshot.ServerTick)
            {
                return;
            }

            EnsureSnapshotRenderRoot();

            List<ulong> staleMarkers = new List<ulong>(snapshotMarkers.Keys);
            for (int i = 0; i < snapshot.Players.Length; ++i)
            {
                RudpPacketCodec.RudpStateSnapshotPlayer player = snapshot.Players[i];
                GameObject marker = EnsureSnapshotMarker(player.SessionId);
                marker.transform.localPosition = new Vector3(
                    player.WorldX * SnapshotRenderScale,
                    player.WorldY * SnapshotRenderScale,
                    SnapshotMarkerZ);
                marker.transform.localScale = new Vector3(
                    SnapshotMarkerSize,
                    SnapshotMarkerSize,
                    SnapshotMarkerSize);

                SpriteRenderer renderer = marker.GetComponent<SpriteRenderer>();
                renderer.color = SnapshotMarkerColor(player.SessionId);
                marker.SetActive(true);
                staleMarkers.Remove(player.SessionId);
            }

            for (int i = 0; i < staleMarkers.Count; ++i)
            {
                ulong sessionId = staleMarkers[i];
                if (snapshotMarkers.TryGetValue(sessionId, out GameObject marker))
                {
                    Destroy(marker);
                    snapshotMarkers.Remove(sessionId);
                }
            }

            hasRenderedSnapshot = true;
            lastRenderSource = renderSource;
            lastRenderedRoomId = snapshot.RoomId;
            lastRenderedTick = snapshot.ServerTick;
            lastRenderedRealtime = Time.realtimeSinceStartup;
            lastRenderedMarkerCount = snapshot.Players.Length;
        }

        private void UpdateCenterItemMarker()
        {
            if (!clientA.IsConnected ||
                !clientB.IsConnected ||
                HasAmbiguousDropSourceForRender() ||
                !TrySelectObservedDropForSmoke(out ObservedDropSourceView dropSource, out _))
            {
                HideCenterItemMarker();
                return;
            }

            if (TrySelectLootResolvedMatchingDrop(dropSource, out _))
            {
                HideCenterItemMarker();
                return;
            }

            EnsureSnapshotRenderRoot();
            GameObject marker = EnsureCenterItemMarker();
            marker.name = "ItemCenter_" + dropSource.DropId;
            marker.transform.localPosition = new Vector3(0f, 0f, CenterItemMarkerZ);
            marker.transform.localScale = new Vector3(
                CenterItemMarkerSize,
                CenterItemMarkerSize,
                CenterItemMarkerSize);
            marker.transform.localRotation = Quaternion.Euler(0f, 0f, 45f);

            SpriteRenderer renderer = marker.GetComponent<SpriteRenderer>();
            renderer.color = new Color(1f, 0.9f, 0.18f, 1f);
            marker.SetActive(true);

            hasRenderedCenterItem = true;
            lastRenderedCenterDropId = dropSource.DropId;
        }

        private void HideCenterItemMarker()
        {
            SetCenterItemMarkerActive(false);
            hasRenderedCenterItem = false;
            lastRenderedCenterDropId = 0;
        }

        private bool HasAmbiguousDropSourceForRender()
        {
            return (clientA.IsConnected && clientA.DropSource.Status == TcpDropSourceStatus.Ambiguous) ||
                   (clientB.IsConnected && clientB.DropSource.Status == TcpDropSourceStatus.Ambiguous);
        }

        private bool TrySelectSnapshotForRender(
            out RudpPacketCodec.RudpStateSnapshotPayload snapshot,
            out string renderSource)
        {
            bool hasA = rudpA.TryLatestStateSnapshot(out RudpPacketCodec.RudpStateSnapshotPayload snapshotA);
            bool hasB = rudpB.TryLatestStateSnapshot(out RudpPacketCodec.RudpStateSnapshotPayload snapshotB);

            if (hasA && hasB)
            {
                if (snapshotA.RoomId == snapshotB.RoomId)
                {
                    if (snapshotA.ServerTick >= snapshotB.ServerTick)
                    {
                        snapshot = snapshotA;
                        renderSource = "A";
                    }
                    else
                    {
                        snapshot = snapshotB;
                        renderSource = "B";
                    }
                    return true;
                }

                if (clientA.RoomId != 0 && snapshotA.RoomId == clientA.RoomId)
                {
                    snapshot = snapshotA;
                    renderSource = "A";
                    return true;
                }
                if (clientB.RoomId != 0 && snapshotB.RoomId == clientB.RoomId)
                {
                    snapshot = snapshotB;
                    renderSource = "B";
                    return true;
                }

                snapshot = snapshotA;
                renderSource = "A";
                return true;
            }

            if (hasA)
            {
                snapshot = snapshotA;
                renderSource = "A";
                return true;
            }
            if (hasB)
            {
                snapshot = snapshotB;
                renderSource = "B";
                return true;
            }

            snapshot = new RudpPacketCodec.RudpStateSnapshotPayload
            {
                Players = new RudpPacketCodec.RudpStateSnapshotPlayer[0]
            };
            renderSource = "-";
            return false;
        }

        private void EnsureSnapshotRenderRoot()
        {
            if (snapshotRenderRoot != null)
            {
                return;
            }

            snapshotRenderRoot = new GameObject("LootOfLegendsSnapshotRenderRoot");
            DontDestroyOnLoad(snapshotRenderRoot);
        }

        private GameObject EnsureSnapshotMarker(ulong sessionId)
        {
            if (snapshotMarkers.TryGetValue(sessionId, out GameObject marker))
            {
                return marker;
            }

            marker = new GameObject("SnapshotPlayer_" + sessionId);
            marker.transform.SetParent(snapshotRenderRoot.transform, false);

            SpriteRenderer renderer = marker.AddComponent<SpriteRenderer>();
            renderer.sprite = SnapshotMarkerSprite();
            renderer.sortingOrder = 10;
            renderer.color = SnapshotMarkerColor(sessionId);

            snapshotMarkers.Add(sessionId, marker);
            return marker;
        }

        private GameObject EnsureCenterItemMarker()
        {
            if (centerItemMarker != null)
            {
                return centerItemMarker;
            }

            centerItemMarker = new GameObject("ItemCenter");
            centerItemMarker.transform.SetParent(snapshotRenderRoot.transform, false);

            SpriteRenderer renderer = centerItemMarker.AddComponent<SpriteRenderer>();
            renderer.sprite = CenterItemMarkerSprite();
            renderer.sortingOrder = 9;
            renderer.color = new Color(1f, 0.9f, 0.18f, 1f);

            return centerItemMarker;
        }

        private Sprite SnapshotMarkerSprite()
        {
            if (snapshotMarkerSprite == null)
            {
                snapshotMarkerSprite = Sprite.Create(
                    Texture2D.whiteTexture,
                    new Rect(0f, 0f, 1f, 1f),
                    new Vector2(0.5f, 0.5f),
                    1f);
            }

            return snapshotMarkerSprite;
        }

        private Sprite CenterItemMarkerSprite()
        {
            if (centerItemMarkerSprite == null)
            {
                centerItemMarkerSprite = Sprite.Create(
                    Texture2D.whiteTexture,
                    new Rect(0f, 0f, 1f, 1f),
                    new Vector2(0.5f, 0.5f),
                    1f);
            }

            return centerItemMarkerSprite;
        }

        private Color SnapshotMarkerColor(ulong sessionId)
        {
            if (sessionId == clientA.SessionId)
            {
                return new Color(0.2f, 0.55f, 1f, 1f);
            }
            if (sessionId == clientB.SessionId)
            {
                return new Color(1f, 0.55f, 0.15f, 1f);
            }

            return new Color(0.65f, 0.65f, 0.65f, 1f);
        }

        private void SetSnapshotMarkersActive(bool active)
        {
            foreach (KeyValuePair<ulong, GameObject> entry in snapshotMarkers)
            {
                if (entry.Value != null)
                {
                    entry.Value.SetActive(active);
                }
            }
        }

        private void ClearSnapshotMarkers()
        {
            foreach (KeyValuePair<ulong, GameObject> entry in snapshotMarkers)
            {
                if (entry.Value != null)
                {
                    Destroy(entry.Value);
                }
            }
            snapshotMarkers.Clear();
        }

        private void SetCenterItemMarkerActive(bool active)
        {
            if (centerItemMarker != null)
            {
                centerItemMarker.SetActive(active);
            }
        }

        private void ClearCenterItemMarker()
        {
            if (centerItemMarker != null)
            {
                Destroy(centerItemMarker);
                centerItemMarker = null;
            }
            hasRenderedCenterItem = false;
            lastRenderedCenterDropId = 0;
        }

        private void DrainSessionLogs(TcpDebugSession session)
        {
            int before = logLines.Count;
            session.DrainLogLines(logLines);
            if (logLines.Count != before)
            {
                TrimLogs();
                logScroll.y = float.MaxValue;
            }
        }

        private void DrainRudpLogs(RudpHelloClient rudpClient)
        {
            int before = logLines.Count;
            rudpClient.DrainLogLines(logLines);
            if (logLines.Count != before)
            {
                TrimLogs();
                logScroll.y = float.MaxValue;
            }
        }

        private void AppendLog(string line)
        {
            logLines.Add(line);
            TrimLogs();
            logScroll.y = float.MaxValue;
        }

        private void TrimLogs()
        {
            while (logLines.Count > MaxLogLines)
            {
                logLines.RemoveAt(0);
            }
        }
    }
}
