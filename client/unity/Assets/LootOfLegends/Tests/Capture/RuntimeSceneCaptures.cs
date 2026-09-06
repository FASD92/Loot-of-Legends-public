using System;
using System.Collections;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Collection;
using LootOfLegends.LobbyRoom;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Presentation.Collection;
using LootOfLegends.Presentation.Common;
using LootOfLegends.Presentation.FinalResult;
using LootOfLegends.Presentation.Lobby;
using LootOfLegends.Presentation.Room;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace LootOfLegends.Tests.Capture
{
    /// <summary>
    /// Drives each screen to a reviewable state and writes a PNG. Every list row and Arena entity
    /// is instantiated from a prefab at runtime, so an Editor Scene capture shows an empty shell
    /// for exactly the content that needs reviewing. These scenarios fill that content from
    /// fabricated read models: no server, no build, and no product code change.
    ///
    /// The scenarios are their own assembly so the product PlayMode filter
    /// (LootOfLegends.Tests.PlayMode) does not pick them up, which keeps the product test count
    /// stable. Without an output directory on the command line each one ignores itself.
    /// </summary>
    public sealed class RuntimeSceneCaptures
    {
        private const ulong LocalSessionId = 1;
        private const ulong HostSessionId = 1;

        [UnityTest]
        public IEnumerator ArenaCombat()
        {
            RuntimeCapture.RequireOutputDirectory();
            SceneManager.LoadScene("ArenaScene");
            yield return null;

            ArenaScreenView view = Object.FindFirstObjectByType<ArenaScreenView>();
            Assert.That(view, Is.Not.Null, "ArenaScene must contain an ArenaScreenView");
            view.SetPlayerContext(LocalSessionId, HostSessionId);
            view.Render(new ArenaPresentationSnapshot(
                false,
                true,
                true,
                false,
                Roster(),
                new ArenaMonsterProjection(true, 1, 2500, 4000, "Alive", string.Empty),
                new[]
                {
                    Drop(11, 1, -3200, 3400),
                    Drop(12, 2, 3600, -1800)
                },
                "Attack: 50 데미지 · 남은 체력 2500",
                "Loot: —",
                new ArenaAttackProjection(7, LocalSessionId, 50, 2500),
                remainingCombatSeconds: 21));
            yield return null;

            yield return RuntimeCapture.Write("arena-combat.png");
        }

        [UnityTest]
        public IEnumerator ArenaLootWindow()
        {
            RuntimeCapture.RequireOutputDirectory();
            SceneManager.LoadScene("ArenaScene");
            yield return null;

            ArenaScreenView view = Object.FindFirstObjectByType<ArenaScreenView>();
            Assert.That(view, Is.Not.Null, "ArenaScene must contain an ArenaScreenView");
            view.SetPlayerContext(LocalSessionId, HostSessionId);
            view.Render(new ArenaPresentationSnapshot(
                false,
                true,
                false,
                true,
                Roster(),
                new ArenaMonsterProjection(false, 1, 0, 4000, "Dead", "MonsterDefeated"),
                new[]
                {
                    Drop(11, 1, -3200, 3400),
                    Drop(12, 2, 3600, -1800),
                    Drop(13, 1, -6800, -2400),
                    Drop(14, 1, 6200, 2600),
                    Drop(15, 1, 800, -5200)
                },
                "Attack: 몬스터 처치",
                "Loot: —",
                remainingLootSeconds: 12));
            // The transient chip and the loot chip are both right aligned but different widths,
            // so showing them together is the only way to review that misalignment.
            view.ShowInputAccepted("전리품 획득 요청이 접수되었습니다.");
            yield return null;

            yield return RuntimeCapture.Write("arena-loot.png");
        }

        [UnityTest]
        public IEnumerator FinalResult()
        {
            RuntimeCapture.RequireOutputDirectory();
            SceneManager.LoadScene("ArenaScene");
            yield return null;

            ArenaScreenView arena = Object.FindFirstObjectByType<ArenaScreenView>();
            Assert.That(arena, Is.Not.Null, "ArenaScene must contain an ArenaScreenView");
            arena.SetPlayerContext(LocalSessionId, HostSessionId);
            // A post combat snapshot retires the waiting overlay and the monster HUD, which is
            // the state the Final Result panel actually appears over.
            arena.Render(new ArenaPresentationSnapshot(
                false,
                false,
                false,
                false,
                Roster(),
                new ArenaMonsterProjection(false, 1, 0, 4000, "Dead", "MonsterDefeated"),
                Array.Empty<ArenaDropProjection>(),
                "Attack: 몬스터 처치",
                "Loot: 전리품 마감"));
            yield return null;

            FinalResultScreenView view = Object.FindFirstObjectByType<FinalResultScreenView>(
                FindObjectsInactive.Include);
            Assert.That(view, Is.Not.Null, "ArenaScene must contain a FinalResultScreenView");
            view.SetPlayerContext(LocalSessionId, HostSessionId);
            view.Show(new FinalResultPresentationSnapshot(
                7,
                9,
                FinalResultPresentationOutcome.MonsterDefeated,
                new[]
                {
                    Row(1, "portfolio-player", 700, 1, true),
                    Row(2, "party-member", 700, 1, true),
                    Row(3, "wandering-ninja", 300, 3, false),
                    Row(4, "quiet-samurai", 100, 4, false),
                    Row(5, "late-arrival", 100, 4, false)
                }));
            view.SetActions(true, "다시 대기실로 돌아갈 수 있습니다.");
            yield return null;

            yield return RuntimeCapture.Write("final-result.png");
        }

        [UnityTest]
        public IEnumerator RoomMembers()
        {
            RuntimeCapture.RequireOutputDirectory();
            SceneManager.LoadScene("RoomScene");
            yield return null;

            RoomScreenView view = Object.FindFirstObjectByType<RoomScreenView>();
            Assert.That(view, Is.Not.Null, "RoomScene must contain a RoomScreenView");
            var readModel = new LobbyRoomReadModel();
            using (var presenter = new RoomPresenter(
                       readModel,
                       new IdleCommands(),
                       new IdleHostStart(),
                       view))
            {
                // Binding is what takes the buttons out of their disabled authoring state, and a
                // washed out button reads as a layout defect that is not there. The presenter's
                // own render runs against an empty read model, so the snapshot below wins.
                view.Bind(presenter, CancellationToken.None);
                presenter.Begin();
                view.Render(new RoomPresentationSnapshot(
                    4,
                    7,
                    "Treasure Hunters",
                    5,
                    new[]
                    {
                        Member(1, "portfolio-player", true, true, true),
                        Member(2, "party-member", true, false, false),
                        Member(3, "wandering-ninja", true, false, false),
                        Member(4, "quiet-samurai", false, false, false),
                        Member(5, "late-arrival", false, false, false)
                    }));
                yield return null;

                yield return RuntimeCapture.Write("room-members.png");
            }
        }

        [UnityTest]
        public IEnumerator SafeFailureNotice()
        {
            RuntimeCapture.RequireOutputDirectory();
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            // 오버레이는 저작 상태에서 꺼져 있고 DontDestroyOnLoad 싱글턴이라, 앞선 시나리오가
            // 만든 인스턴스가 살아있을 수 있다. 비활성 포함으로 찾아야 한다.
            SafeFailureTextView view = Object.FindFirstObjectByType<SafeFailureTextView>(
                FindObjectsInactive.Include);
            Assert.That(view, Is.Not.Null,
                "LobbyScene must contain a SafeFailureTextView");

            // SafeFailurePresenter.SessionReplacedCopy 와 같은 실제 제품 문구를 쓴다.
            view.ShowBlockingMessage("다른 로그인으로 현재 세션이 종료되었습니다.");
            yield return null;

            yield return RuntimeCapture.Write("safe-failure.png");
        }

        [UnityTest]
        public IEnumerator LobbyRooms()
        {
            RuntimeCapture.RequireOutputDirectory();
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            LobbyScreenView view = Object.FindFirstObjectByType<LobbyScreenView>();
            Assert.That(view, Is.Not.Null, "LobbyScene must contain a LobbyScreenView");
            var readModel = new LobbyRoomReadModel();
            using (var presenter = new LobbyPresenter(readModel, new IdleCommands(), view))
            {
                view.Bind(presenter, CancellationToken.None);
                presenter.Begin();
                view.Render(new LobbyPresentationSnapshot(
                    3,
                    "portfolio-player",
                    new[]
                    {
                        new LobbyRoomSummaryView(7, "Treasure Hunters", 3, 5),
                        new LobbyRoomSummaryView(8, "Full Expedition", 4, 4),
                        new LobbyRoomSummaryView(9, "First Timers", 1, 10)
                    }));
                yield return null;

                yield return RuntimeCapture.Write("lobby-rooms.png");
            }
        }

        [UnityTest]
        public IEnumerator CollectionItems()
        {
            RuntimeCapture.RequireOutputDirectory();
            SceneManager.LoadScene("LobbyScene");
            yield return null;

            CollectionScreenView view = Object.FindFirstObjectByType<CollectionScreenView>(
                FindObjectsInactive.Include);
            Assert.That(view, Is.Not.Null, "LobbyScene must contain a CollectionScreenView");
            view.Bind(() => { });
            RevealCollectionPanel(view);
            view.Render(new CollectionPresentationSnapshot(
                CollectionPresentationState.Fresh,
                new[]
                {
                    new CollectionPresentationItem(1, 7, 100),
                    new CollectionPresentationItem(2, 2, 300)
                },
                1300,
                1));
            yield return null;

            yield return RuntimeCapture.Write("collection.png");
        }

        private static ArenaPlayerProjection[] Roster()
        {
            // The server clamps positions to +-10000mm, which is world [-10, 10]. Keeping the
            // roster inside that range means the capture shows reachable ground only.
            return new[]
            {
                new ArenaPlayerProjection(1, -4200, -2600, "portfolio-player"),
                new ArenaPlayerProjection(2, 2800, -3400, "party-member"),
                new ArenaPlayerProjection(3, -6400, 1800, "wandering-ninja"),
                new ArenaPlayerProjection(4, 5600, 2200, "quiet-samurai"),
                new ArenaPlayerProjection(5, 400, -6000, "late-arrival")
            };
        }

        private static ArenaDropProjection Drop(ulong dropId, ulong itemId, int x, int y)
        {
            return new ArenaDropProjection(dropId, itemId, 1, x, y, "Available", 0);
        }

        private static FinalResultPresentationRow Row(
            ulong sessionId,
            string nickname,
            ulong value,
            uint? rank,
            bool isTop)
        {
            return new FinalResultPresentationRow(
                sessionId,
                nickname,
                FinalResultPresentationExitStatus.TerminalPresent,
                value,
                rank,
                isTop);
        }

        private static RoomMemberPresentation Member(
            ulong sessionId,
            string nickname,
            bool ready,
            bool isLocal,
            bool isHost)
        {
            return new RoomMemberPresentation(
                sessionId,
                sessionId + 1,
                nickname,
                ready,
                isLocal,
                isHost);
        }

        /// <summary>
        /// The Collection panel starts collapsed behind its toggle, and both the CanvasGroup field
        /// and the method that drives it are private to the View. Reflection is how the existing
        /// PlayMode tests reach private SerializeFields, and reusing the View's own method keeps
        /// the captured state identical to what a player sees after pressing the toggle.
        /// </summary>
        private static void RevealCollectionPanel(CollectionScreenView view)
        {
            MethodInfo reveal = typeof(CollectionScreenView).GetMethod(
                "SetPanelVisible",
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(reveal, Is.Not.Null, "CollectionScreenView.SetPanelVisible is missing");
            reveal.Invoke(view, new object[] { true });
        }

        private sealed class IdleCommands : ILobbyRoomCommands
        {
            public Task<RoomCommandResult> CreateAsync(
                string title,
                byte capacity,
                CancellationToken cancellationToken)
            {
                return Pending();
            }

            public Task<RoomCommandResult> JoinAsync(
                ulong roomId,
                CancellationToken cancellationToken)
            {
                return Pending();
            }

            public Task<RoomCommandResult> LeaveAsync(CancellationToken cancellationToken)
            {
                return Pending();
            }

            public Task<RoomCommandResult> SetReadyAsync(
                bool ready,
                CancellationToken cancellationToken)
            {
                return Pending();
            }

            public Task<RoomCommandResult> KickAsync(
                ulong targetSessionId,
                ulong targetSessionGeneration,
                CancellationToken cancellationToken)
            {
                return Pending();
            }

            // A capture never presses a button, so a command that never completes is the honest
            // stand in: it cannot move the screen out of the state being reviewed.
            private static Task<RoomCommandResult> Pending()
            {
                return new TaskCompletionSource<RoomCommandResult>().Task;
            }
        }

        private sealed class IdleHostStart : IRoomHostStartAction
        {
            public Task<RoomCommandResult> StartAsync(CancellationToken cancellationToken)
            {
                return new TaskCompletionSource<RoomCommandResult>().Task;
            }
        }
    }
}
