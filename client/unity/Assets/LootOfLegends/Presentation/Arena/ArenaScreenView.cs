using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using UnityEngine;
using UnityEngine.UI;

namespace LootOfLegends.Presentation.Arena
{
    public sealed class ArenaScreenView : MonoBehaviour, IArenaView
    {
        // One verb per keyboard cap shown beside it. The cap art carries the key name, so spelling
        // "WASD" or "Space" here would say it twice.
        private const string MoveHint = "이동";
        private const string AttackHint = "공격";
        private const string ClaimHint = "획득";

        // 이 문구는 초기값이면서 "아직 서버 Arena 상태가 없다"는 판정 기준으로도 쓰인다.
        // 두 곳에 리터럴로 두면 한쪽만 고쳐졌을 때 transient 상태 표시가 조용히 어긋난다.
        private const string WaitingForArenaCopy = "서버 아레나 상태를 기다리는 중입니다.";

        [SerializeField] private GameObject panel;
        [SerializeField] private Text statusLabel;
        [SerializeField] private GameObject monsterHudPanel;
        [SerializeField] private Text monsterHitPointsLabel;
        [SerializeField] private Image monsterHitPointsFill;
        [SerializeField] private Text transientStatusLabel;
        [SerializeField] private Text terminalCopyLabel;
        [SerializeField] private Text lootCountLabel;
        [SerializeField] private Text inputHintLabel;
        [SerializeField] private Text attackHintLabel;
        [SerializeField] private Text claimHintLabel;
        [SerializeField] private ArenaWorldView worldView;
        [SerializeField] private BattleLoadWaitingOverlay waitingOverlay;

        private ArenaPresenter presenter;
        private CancellationToken cancellationToken;
        private ArenaPresentationSnapshot snapshot;
        private Task pending;

        // 트랙은 SerializeField 로 묶지 않고 fill 에서 찾아 캐시한다. 씬 바인딩을 하나 더
        // 늘리지 않고, 프리팹 계층 깊이가 바뀌어도 따라간다.
        private Transform monsterHealthTrack;
        private bool monsterHealthTrackResolved;
        private string statusCopy = WaitingForArenaCopy;

        public ArenaPresentationSnapshot Snapshot => snapshot;
        public string StatusCopy => statusCopy;

        public void SetPlayerContext(ulong localSessionId, ulong hostSessionId)
        {
            worldView?.SetPlayerContext(localSessionId, hostSessionId);
        }

        private void Awake()
        {
            RefreshView();
        }

        public void Bind(
            ArenaPresenter screenPresenter,
            CancellationToken screenCancellationToken)
        {
            presenter = screenPresenter ??
                throw new ArgumentNullException(nameof(screenPresenter));
            cancellationToken = screenCancellationToken;
        }

        public void Render(ArenaPresentationSnapshot next)
        {
            snapshot = next ?? throw new ArgumentNullException(nameof(next));
            worldView?.Render(snapshot);
            RefreshView();
        }

        public void ShowInputAccepted(string copy)
        {
            statusCopy = copy ?? string.Empty;
            RefreshStatus();
        }

        public Task MoveAsync(short desiredX, short desiredY)
        {
            EnsureCanBegin();
            statusCopy = "이동 요청 중입니다.";
            RefreshStatus();
            pending = presenter.MoveAsync(desiredX, desiredY, cancellationToken);
            return pending;
        }

        public Task AttackAsync(ulong targetId)
        {
            EnsureCanBegin();
            statusCopy = "공격 요청 중입니다.";
            RefreshStatus();
            pending = presenter.AttackAsync(targetId, cancellationToken);
            return pending;
        }

        public Task ClaimAsync(ulong dropId)
        {
            EnsureCanBegin();
            statusCopy = "획득 요청 중입니다.";
            RefreshStatus();
            pending = presenter.ClaimAsync(dropId, cancellationToken);
            return pending;
        }

        private void Update()
        {
            if (pending == null || !pending.IsCompleted)
            {
                return;
            }
            if (pending.IsFaulted || pending.IsCanceled)
            {
                statusCopy = "요청을 완료하지 못했습니다.";
                RefreshStatus();
            }
            pending = null;
        }

        private void RefreshView()
        {
            bool waiting = snapshot == null || snapshot.WaitingForGameplayStart;
            bool monsterAlive = snapshot?.Monster.HasMonster == true &&
                snapshot.Monster.State == "Alive";
            bool lootOpen = snapshot?.CanClaimLoot == true;
            waitingOverlay?.Render(waiting);
            panel?.SetActive(snapshot != null && !waiting);
            monsterHudPanel?.SetActive(monsterAlive || lootOpen);
            RefreshStatus();
            if (inputHintLabel != null)
            {
                bool showHint = snapshot?.ControlsEnabled == true && !waiting;
                inputHintLabel.text = MoveHint;
                if (attackHintLabel != null)
                {
                    attackHintLabel.text = AttackHint;
                }
                if (claimHintLabel != null)
                {
                    claimHintLabel.text = ClaimHint;
                }
                // The keyboard caps are siblings of these labels, so the whole hint panel has to be
                // the thing that hides rather than any single label.
                inputHintLabel.transform.parent.gameObject.SetActive(showHint);
            }
            if (monsterHitPointsLabel != null)
            {
                monsterHitPointsLabel.text = monsterAlive
                    ? $"몬스터\n{snapshot.Monster.HitPoints} / " +
                        snapshot.Monster.MaximumHitPoints + " · " +
                        $"{snapshot.RemainingCombatSeconds / 60:00}:" +
                        $"{snapshot.RemainingCombatSeconds % 60:00}"
                    : lootOpen
                        ? $"전리품 마감\n{snapshot.RemainingLootSeconds / 60:00}:" +
                            $"{snapshot.RemainingLootSeconds % 60:00}"
                        : "몬스터\n—";
            }
            if (monsterHitPointsFill != null)
            {
                float ratio = MonsterHitPointRatio();
                monsterHitPointsFill.fillAmount = ratio;
                monsterHitPointsFill.rectTransform.anchorMax =
                    new Vector2(ratio, 1f);
                monsterHitPointsFill.gameObject.SetActive(ratio > 0f);

                // 전리품 단계에서는 이 패널이 몬스터 체력이 아니라 전리품 마감 카운트다운을
                // 표시한다. fill 만 숨기면 체력 트랙이 의미 없는 빈 바로 남고, 패널 전체를
                // 숨기면 카운트다운까지 사라진다. 그래서 트랙만 숨긴다.
                Transform track = ResolveMonsterHealthTrack();
                if (track != null)
                {
                    track.gameObject.SetActive(monsterAlive);
                }
            }
            if (terminalCopyLabel != null)
            {
                // 이 라벨은 서버가 보낸 result code 를 그대로 비춰 client 가 상태를 예측하지
                // 않는다는 것을 화면으로 보여주는 개발용 표시다. 값이 22종의 protocol 코드
                // (Ok, Cooldown, StaleSession, CommandConflict ...)이고 플레이어가 할 수 있는
                // 동작이 없어서 ArenaHudPanel 프리팹에서 ArenaTerminalPanel 을 비활성으로
                // 저작했다. 플레이어용 피드백은 transientStatusLabel 이 한국어로 담당한다.
                // 여기서 계속 갱신하는 이유는 에디터에서 패널만 켜면 바로 다시 볼 수 있게
                // 두려는 것이다. 서버 authority 단정은 snapshot 의 AttackTerminalCopy /
                // LootTerminalCopy 를 검사하므로 이 표시와 무관하게 유지된다.
                terminalCopyLabel.text = TerminalCopy(
                    snapshot?.AttackTerminalCopy,
                    "Attack") + "\n" + TerminalCopy(snapshot?.LootTerminalCopy, "Loot");
            }
            if (lootCountLabel != null)
            {
                lootCountLabel.text = "전리품 " +
                    (snapshot?.CanClaimLoot == true ? AvailableDropCount() : 0);
            }
        }

        private void RefreshStatus()
        {
            if (statusLabel == null)
            {
                return;
            }
            statusLabel.text = HighLevelStatus();
            if (transientStatusLabel != null)
            {
                bool show = snapshot != null && !snapshot.WaitingForGameplayStart &&
                    statusCopy != WaitingForArenaCopy;
                transientStatusLabel.text = show ? statusCopy : string.Empty;
                transientStatusLabel.transform.parent.gameObject.SetActive(show);
            }
        }

        private string HighLevelStatus()
        {
            if (snapshot == null || snapshot.WaitingForGameplayStart)
            {
                return "전투 준비 중";
            }
            if (snapshot.CanClaimLoot && AvailableDropCount() > 0)
            {
                return "전리품 획득 가능";
            }
            if (snapshot.Monster.HasMonster && snapshot.CanAttack)
            {
                return "전투 진행 중";
            }
            return snapshot.ControlsEnabled
                ? "전투 상태 동기화 중"
                : "전투 준비 중";
        }

        private int AvailableDropCount()
        {
            return snapshot?.Drops.Count(drop => drop.State == "Available") ?? 0;
        }

        private float MonsterHitPointRatio()
        {
            if (snapshot?.Monster.HasMonster != true ||
                snapshot.Monster.State != "Alive" ||
                snapshot.Monster.MaximumHitPoints == 0)
            {
                return 0f;
            }
            return Mathf.Clamp01(
                (float)snapshot.Monster.HitPoints / snapshot.Monster.MaximumHitPoints);
        }

        /// <summary>
        /// 체력 트랙은 fill 의 조상 중 몬스터 HUD 패널의 직속 자식이다. 부모를 한 단계씩 올라가
        /// 찾으므로 fill 과 트랙 사이에 컨테이너가 늘거나 줄어도 따라간다. 고정된
        /// <c>parent.parent</c> 로 잡으면 계층이 얕아지는 순간 패널 자체를 숨겨서 전리품 마감
        /// 카운트다운이 조용히 사라진다.
        /// </summary>
        private Transform ResolveMonsterHealthTrack()
        {
            if (monsterHealthTrackResolved)
            {
                return monsterHealthTrack;
            }
            monsterHealthTrackResolved = true;
            if (monsterHitPointsFill == null || monsterHudPanel == null)
            {
                return null;
            }

            Transform panel = monsterHudPanel.transform;
            Transform candidate = monsterHitPointsFill.transform;
            while (candidate.parent != null && candidate.parent != panel)
            {
                candidate = candidate.parent;
            }
            monsterHealthTrack = candidate.parent == panel ? candidate : null;
            return monsterHealthTrack;
        }

        private static string TerminalCopy(string copy, string label)
        {
            return string.IsNullOrEmpty(copy) ? label + ": —" : copy;
        }

        private void EnsureCanBegin()
        {
            if (presenter == null)
            {
                throw new InvalidOperationException("Arena screen is not bound");
            }
            if (pending != null)
            {
                throw new InvalidOperationException("Arena request is already pending");
            }
        }
    }
}
