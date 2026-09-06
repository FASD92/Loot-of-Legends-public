using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.Battle.Combat;
using LootOfLegends.Battle.Loot;
using LootOfLegends.Battle.Movement;
using LootOfLegends.Presentation;
using LootOfLegends.Presentation.Arena;
using LootOfLegends.Protocol;
using LootOfLegends.Transport.Rudp;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using UnityEngine.UI;
using Object = UnityEngine.Object;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class ArenaNormalFlowPresentationTests
    {
        [UnityTest]
        public IEnumerator PlayerVisualUsesHorizontalPriorityAndKeepsIdleFacing()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            var owner = new GameObject("ArenaPlayerVisualTest");
            SpriteRenderer renderer = owner.AddComponent<SpriteRenderer>();
            ArenaPlayerVisual visual = owner.AddComponent<ArenaPlayerVisual>();
            try
            {
                visual.Bind(appearances, 0);
                visual.Project(Vector3.zero);
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Down, 0)));

                visual.Project(new Vector3(-1f, 1f));
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Left, 0)));
                visual.Project(new Vector3(-2f, 0f));
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Left, 0)));

                visual.Project(new Vector3(-1f, 1f));
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Right, 0)));
                visual.Project(Vector3.zero);
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Right, 0)));

                visual.Project(new Vector3(0f, 1f));
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Up, 0)));
                visual.Project(Vector3.zero);
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Down, 0)));
                visual.Project(Vector3.zero);
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Down, 0)));
                yield return null;
            }
            finally
            {
                Object.DestroyImmediate(owner);
                DestroyOwned(owned);
            }
        }

        [UnityTest]
        public IEnumerator PlayerVisualAdvancesWalkFrames()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            var owner = new GameObject("ArenaPlayerAnimationTest");
            SpriteRenderer renderer = owner.AddComponent<SpriteRenderer>();
            ArenaPlayerVisual visual = owner.AddComponent<ArenaPlayerVisual>();
            try
            {
                visual.Bind(appearances, 0);
                visual.Project(Vector3.zero);
                visual.Project(Vector3.right);
                Sprite first = renderer.sprite;

                yield return new WaitForSeconds(0.2f);

                Assert.That(renderer.sprite, Is.Not.SameAs(first));
                Assert.That(renderer.sprite,
                    Is.SameAs(appearances.Resolve(0, ArenaFacing.Right, 1)));
            }
            finally
            {
                Object.DestroyImmediate(owner);
                DestroyOwned(owned);
            }
        }

        [Test]
        public void WorldAssignsTenStableSlotsBySortedSessionId()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            ArenaWorldView view = CreateAppearanceWorld(
                appearances,
                out GameObject owner,
                out GameObject playerPrefab);
            try
            {
                ArenaPlayerProjection[] first = Enumerable.Range(1, 10)
                    .Reverse()
                    .Select(id => new ArenaPlayerProjection(
                        (ulong)id,
                        id * 1000,
                        id * -1000))
                    .ToArray();
                view.Render(PlayerSnapshot(first));

                for (int id = 1; id <= 10; id++)
                {
                    SpriteRenderer renderer = owner.transform.Find("Player-" + id)
                        .GetComponent<SpriteRenderer>();
                    Assert.That(renderer.sprite,
                        Is.SameAs(appearances.Resolve(
                            id - 1,
                            ArenaFacing.Down,
                            0)));
                }

                view.Render(PlayerSnapshot(first.Reverse().ToArray()));
                for (int id = 1; id <= 10; id++)
                {
                    Assert.That(
                        owner.transform.Find("Player-" + id)
                            .GetComponent<SpriteRenderer>().sprite,
                        Is.SameAs(appearances.Resolve(
                            id - 1,
                            ArenaFacing.Down,
                            0)));
                }
            }
            finally
            {
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(playerPrefab);
                DestroyOwned(owned);
            }
        }

        [Test]
        public void WorldShowsServerParticipantNicknameAbovePlayer()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            ArenaWorldView view = CreateAppearanceWorld(
                appearances,
                out GameObject owner,
                out GameObject playerPrefab);
            try
            {
                view.SetPlayerContext(1, 1);
                view.Render(PlayerSnapshot(
                    new ArenaPlayerProjection(1, 0, 0, "neo")));

                TextMesh label = owner.transform.Find("Player-1")
                    .GetComponentInChildren<TextMesh>();
                Assert.That(label, Is.Not.Null);
                Assert.That(label.text, Is.EqualTo("neo (나·방장)"));
                Assert.That(label.fontSize, Is.GreaterThanOrEqualTo(48));
                Assert.That(label.characterSize, Is.GreaterThanOrEqualTo(0.06f));
            }
            finally
            {
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(playerPrefab);
                DestroyOwned(owned);
            }
        }

        [Test]
        public void WorldOutlinesNicknameForContrastOverAnyBackground()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            ArenaWorldView view = CreateAppearanceWorld(
                appearances,
                out GameObject owner,
                out GameObject playerPrefab);
            try
            {
                view.SetPlayerContext(1, 1);
                view.Render(PlayerSnapshot(
                    new ArenaPlayerProjection(1, 0, 0, "neo")));

                Transform player = owner.transform.Find("Player-1");

                // 깊이 우선 탐색이 본체를 먼저 만나야 한다. 외곽선이 먼저 잡히면 기존
                // 닉네임 단정이 조용히 외곽선을 검사하게 된다.
                TextMesh label = player.GetComponentInChildren<TextMesh>();
                Assert.That(label.name, Is.EqualTo("Nickname"));

                TextMesh[] outline = player.GetComponentsInChildren<TextMesh>(true)
                    .Where(mesh => mesh != label)
                    .ToArray();

                // 대각을 포함한 8 방향. 4 방향만 두면 글자 대각선에 구멍이 남는다.
                Assert.That(outline, Has.Length.EqualTo(8));
                Assert.That(
                    outline.Select(mesh => mesh.transform.parent),
                    Is.All.SameAs(label.transform));
                Assert.That(outline.Select(mesh => mesh.text),
                    Is.All.EqualTo("neo (나·방장)"));

                // 외곽선이 본체보다 먼저 그려져야 글자를 덮지 않는다.
                int labelOrder = label.GetComponent<MeshRenderer>().sortingOrder;
                Assert.That(
                    outline.Select(mesh =>
                        mesh.GetComponent<MeshRenderer>().sortingOrder),
                    Is.All.LessThan(labelOrder));

                // 8 방향이 서로 다른 오프셋을 가져야 실제로 글자를 감싼다.
                Vector2[] offsets = outline
                    .Select(mesh => (Vector2)mesh.transform.localPosition)
                    .ToArray();
                Assert.That(offsets.Distinct().ToArray(), Has.Length.EqualTo(8));
                Assert.That(offsets, Is.All.Matches<Vector2>(
                    offset => offset != Vector2.zero));

                // 판독성의 근거는 글자와 외곽선의 상대휘도 대비다. 단색으로는 바닥
                // 2.54:1 / 스프라이트 1.63:1 밖에 안 나왔다. WCAG AA 일반 텍스트 4.5:1 이상.
                double contrast = ContrastRatio(
                    label.color,
                    outline[0].color);
                Assert.That(contrast, Is.GreaterThanOrEqualTo(4.5d),
                    "닉네임 글자와 외곽선의 대비가 부족하면 배경과 무관하게 뭉개진다");
            }
            finally
            {
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(playerPrefab);
                DestroyOwned(owned);
            }
        }

        private static double ContrastRatio(Color first, Color second)
        {
            double a = RelativeLuminance(first);
            double b = RelativeLuminance(second);
            double lighter = System.Math.Max(a, b);
            double darker = System.Math.Min(a, b);
            return (lighter + 0.05d) / (darker + 0.05d);
        }

        private static double RelativeLuminance(Color color)
        {
            return 0.2126d * Linear(color.r)
                + 0.7152d * Linear(color.g)
                + 0.0722d * Linear(color.b);
        }

        private static double Linear(float channel)
        {
            return channel <= 0.04045f
                ? channel / 12.92d
                : System.Math.Pow((channel + 0.055d) / 1.055d, 2.4d);
        }

        [Test]
        public void WorldRejectsElevenPlayerRoster()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            ArenaWorldView view = CreateAppearanceWorld(
                appearances,
                out GameObject owner,
                out GameObject playerPrefab);
            try
            {
                ArenaPlayerProjection[] players = Enumerable.Range(1, 11)
                    .Select(id => new ArenaPlayerProjection((ulong)id, 0, 0))
                    .ToArray();
                Assert.Throws<InvalidOperationException>(() =>
                    view.Render(PlayerSnapshot(players)));
            }
            finally
            {
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(playerPrefab);
                DestroyOwned(owned);
            }
        }

        [Test]
        public void WorldRejectsUnknownPlayerAfterRosterInitialization()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            ArenaWorldView view = CreateAppearanceWorld(
                appearances,
                out GameObject owner,
                out GameObject playerPrefab);
            try
            {
                view.Render(PlayerSnapshot(
                    new ArenaPlayerProjection(1, 0, 0),
                    new ArenaPlayerProjection(2, 0, 0)));

                Assert.Throws<InvalidOperationException>(() => view.Render(PlayerSnapshot(
                    new ArenaPlayerProjection(1, 0, 0),
                    new ArenaPlayerProjection(2, 0, 0),
                    new ArenaPlayerProjection(3, 0, 0))));
            }
            finally
            {
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(playerPrefab);
                DestroyOwned(owned);
            }
        }

        [UnityTest]
        public IEnumerator WorldShowsEachServerAppliedAttackOnce()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            ArenaWorldView view = CreateAppearanceWorld(
                appearances,
                out GameObject owner,
                out GameObject playerPrefab);
            var monsterPrefab = new GameObject("MonsterPrefab");
            monsterPrefab.AddComponent<SpriteRenderer>();
            SetPrefab(view, "monsterPrefab", monsterPrefab);
            try
            {
                var players = new[]
                {
                    new ArenaPlayerProjection(1, -1000, 0),
                    new ArenaPlayerProjection(2, 1000, 0)
                };
                var attack = new ArenaAttackProjection(1, 1, 50, 1550);
                var snapshot = new ArenaPresentationSnapshot(
                    false,
                    true,
                    true,
                    false,
                    players,
                    new ArenaMonsterProjection(true, 1, 1550, 1600, "Alive", ""),
                    Array.Empty<ArenaDropProjection>(),
                    "Attack: Ok",
                    "Loot: —",
                    attack);

                view.Render(snapshot);
                Assert.That(owner.transform.Find("AttackEffect-1"), Is.Not.Null);
                Assert.That(
                    owner.transform.Find("AttackEffect-1")
                        .GetComponent<LineRenderer>().startWidth,
                    Is.GreaterThanOrEqualTo(0.14f));
                view.Render(snapshot);
                Assert.That(owner.GetComponentsInChildren<ArenaAttackProjectile>(),
                    Has.Length.EqualTo(1));
                yield return new WaitForSeconds(0.2f);
                Assert.That(
                    owner.transform.Find("Monster-1").GetComponent<SpriteRenderer>().color,
                    Is.Not.EqualTo(Color.white));

                view.Render(new ArenaPresentationSnapshot(
                    false,
                    true,
                    true,
                    false,
                    players,
                    new ArenaMonsterProjection(true, 1, 1500, 1600, "Alive", ""),
                    Array.Empty<ArenaDropProjection>(),
                    "Attack: Ok",
                    "Loot: —",
                    new ArenaAttackProjection(2, 2, 50, 1500)));
                yield return new WaitForSeconds(0.35f);

                Assert.That(owner.GetComponentsInChildren<ArenaAttackProjectile>(),
                    Is.Empty);
                Assert.That(
                    owner.transform.Find("Monster-1").GetComponent<SpriteRenderer>().color,
                    Is.EqualTo(Color.white));
            }
            finally
            {
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(playerPrefab);
                Object.DestroyImmediate(monsterPrefab);
                DestroyOwned(owned);
            }
        }

        [UnityTest]
        public IEnumerator ProductWorldPlaysServerConfirmedCombatAndLootSounds()
        {
            SceneManager.LoadScene("ArenaScene");
            yield return null;
            ArenaWorldView view = Object.FindFirstObjectByType<ArenaWorldView>();
            AudioSource audioSource = view.GetComponent<AudioSource>();
            Assert.That(audioSource, Is.Not.Null);

            var players = new[] { new ArenaPlayerProjection(1, -1000, 0) };
            view.Render(new ArenaPresentationSnapshot(
                false, true, true, false, players,
                new ArenaMonsterProjection(true, 1, 1600, 1600, "Alive", ""),
                Array.Empty<ArenaDropProjection>(), "Attack: —", "Loot: —"));
            audioSource.Stop();

            view.Render(new ArenaPresentationSnapshot(
                false, true, true, false, players,
                new ArenaMonsterProjection(true, 1, 1550, 1600, "Alive", ""),
                Array.Empty<ArenaDropProjection>(), "Attack: Ok", "Loot: —",
                new ArenaAttackProjection(1, 1, 50, 1550)));
            Assert.That(audioSource.isPlaying, Is.True, "attack");

            audioSource.Stop();
            yield return new WaitForSeconds(0.2f);
            Assert.That(audioSource.isPlaying, Is.True, "hit");

            audioSource.Stop();
            view.Render(new ArenaPresentationSnapshot(
                false, true, false, true, players,
                new ArenaMonsterProjection(true, 1, 0, 1600, "Dead", "Defeated"),
                Array.Empty<ArenaDropProjection>(), "Attack: Defeated", "Loot: —",
                new ArenaAttackProjection(1, 1, 50, 1550)));
            Assert.That(audioSource.isPlaying, Is.True, "monster death");

            audioSource.Stop();
            view.Render(new ArenaPresentationSnapshot(
                false, true, false, true, players,
                new ArenaMonsterProjection(false, 0, 0, 0, "", ""),
                new[] { new ArenaDropProjection(3, 2, 1, 1000, 0, "Available", 0) },
                "Attack: Defeated", "Loot: —"));
            audioSource.Stop();
            view.Render(new ArenaPresentationSnapshot(
                false, true, false, false, players,
                new ArenaMonsterProjection(false, 0, 0, 0, "", ""),
                new[] { new ArenaDropProjection(3, 2, 1, 1000, 0, "Claimed", 1) },
                "Attack: Defeated", "Loot: Ok"));
            Assert.That(audioSource.isPlaying, Is.True, "loot pickup");
        }

        [UnityTest]
        public IEnumerator WorldUsesExactServerPositionsAndRemovesAbsentEntities()
        {
            var owned = new List<Object>();
            ArenaPlayerAppearanceSet appearances = CreateTestAppearances(owned);
            var owner = new GameObject("ArenaWorldViewTest");
            var playerPrefab = new GameObject("PlayerPrefab");
            playerPrefab.AddComponent<SpriteRenderer>();
            playerPrefab.AddComponent<ArenaPlayerVisual>();
            var monsterPrefab = new GameObject("MonsterPrefab");
            var dropPrefab = new GameObject("DropPrefab");
            dropPrefab.AddComponent<SpriteRenderer>();
            dropPrefab.AddComponent<ArenaDropVisual>();
            var view = owner.AddComponent<ArenaWorldView>();
            SetPrefab(view, "playerPrefab", playerPrefab);
            SetPrefab(view, "monsterPrefab", monsterPrefab);
            SetPrefab(view, "dropPrefab", dropPrefab);
            SetAppearance(view, appearances);
            try
            {
                view.Render(new ArenaPresentationSnapshot(
                    false,
                    true,
                    true,
                    true,
                    new[] { new ArenaPlayerProjection(1, 1250, -1750) },
                    new ArenaMonsterProjection(true, 9, 1600, 2000, "Alive", ""),
                    new[]
                    {
                        new ArenaDropProjection(3, 2, 1, 3000, 4000, "Available", 0),
                        new ArenaDropProjection(4, 2, 1, 9000, 9000, "Claimed", 1)
                    },
                    "Attack: —",
                    "Loot: —"));

                Assert.That(owner.transform.Find("Player-1").localPosition,
                    Is.EqualTo(new Vector3(1.25f, -1.75f, 0f)));
                Assert.That(owner.transform.Find("Monster-9").localPosition,
                    Is.EqualTo(Vector3.zero));
                Assert.That(owner.transform.Find("Drop-3").localPosition,
                    Is.EqualTo(new Vector3(3f, 4f, 0f)));
                Assert.That(owner.transform.Find("Drop-4"), Is.Null);

                view.Render(new ArenaPresentationSnapshot(
                    false,
                    true,
                    false,
                    false,
                    new ArenaPlayerProjection[0],
                    new ArenaMonsterProjection(false, 0, 0, 0, "", ""),
                    new ArenaDropProjection[0],
                    "Attack: —",
                    "Loot: —"));
                yield return null;

                Assert.That(owner.transform.childCount, Is.Zero);
            }
            finally
            {
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(playerPrefab);
                Object.DestroyImmediate(monsterPrefab);
                Object.DestroyImmediate(dropPrefab);
                DestroyOwned(owned);
            }
        }

        [UnityTest]
        public IEnumerator WaitingOverlayBlocksAllInputsUntilServerGameplayStart()
        {
            var load = new BattleLoadReadModel();
            var combat = new BattleCombatReadModel(9);
            var loot = new BattleLootReadModel(9);
            int moves = 0;
            int attacks = 0;
            int claims = 0;
            var input = new ArenaInputFacade(
                load,
                combat,
                loot,
                (x, y, cancellation) => { moves++; return Task.CompletedTask; },
                (target, cancellation) => { attacks++; return Task.CompletedTask; },
                (drop, cancellation) => { claims++; return Task.CompletedTask; });
            var owner = new GameObject("ArenaScreenViewTest");
            var view = owner.AddComponent<ArenaScreenView>();
            try
            {
                var presenter = new ArenaPresenter(
                    new ArenaPlayerFlowReadModel(
                        load,
                        new BattleMovementReadModel(9),
                        combat,
                        loot),
                    input,
                    view);
                view.Bind(presenter, CancellationToken.None);

                load.Apply(new ArenaLoadEntry(7, 9));
                presenter.Render();
                Assert.That(view.Snapshot.WaitingForGameplayStart, Is.True);
                Assert.That(view.Snapshot.ControlsEnabled, Is.False);
                Assert.ThrowsAsync<ArenaInputUnavailableException>(async () =>
                    await presenter.MoveAsync(1, 0, CancellationToken.None));
                Assert.ThrowsAsync<ArenaInputUnavailableException>(async () =>
                    await presenter.AttackAsync(1, CancellationToken.None));
                Assert.ThrowsAsync<ArenaInputUnavailableException>(async () =>
                    await presenter.ClaimAsync(1, CancellationToken.None));
                Assert.That(moves + attacks + claims, Is.Zero);

                load.Apply(GameplayStart());
                combat.Apply(new RudpMonsterSpawned(
                    new RudpEventId(1, 1),
                    9,
                    RudpEventStreamKind.CombatLifecycle,
                    1,
                    1,
                    0,
                    0,
                    1600,
                    4));
                loot.Apply(new RudpDropStateSnapshot(
                    9,
                    1,
                    RudpLootResolutionState.Open,
                    new[]
                    {
                        new RudpLootDropProjection(
                            3, 2, 1, 1000, 2000, RudpLootDropState.Available, 0)
                    }));
                presenter.Render();
                Assert.That(view.Snapshot.ControlsEnabled, Is.True);

                Task move = view.MoveAsync(1, -1);
                yield return new WaitUntil(() => move.IsCompleted);
                yield return null;
                Task attack = view.AttackAsync(1);
                yield return new WaitUntil(() => attack.IsCompleted);
                yield return null;
                Task claim = view.ClaimAsync(3);
                yield return new WaitUntil(() => claim.IsCompleted);

                Assert.That(moves, Is.EqualTo(1));
                Assert.That(attacks, Is.EqualTo(1));
                Assert.That(claims, Is.EqualTo(1));
                Assert.That(view.Snapshot.Players, Is.Empty,
                    "input submission must not create a predicted position");
            }
            finally
            {
                Object.DestroyImmediate(owner);
            }
        }

        [UnityTest]
        public IEnumerator SceneRendersWaitingCombatAndLootHudFromSnapshots()
        {
            SceneManager.LoadScene("ArenaScene");
            yield return null;

            ArenaScreenView view = Object.FindFirstObjectByType<ArenaScreenView>();
            BattleLoadWaitingOverlay overlay = Object.FindFirstObjectByType<
                BattleLoadWaitingOverlay>(FindObjectsInactive.Include);
            Assert.That(view, Is.Not.Null);
            Assert.That(overlay, Is.Not.Null);
            CanvasGroup waiting = overlay.GetComponent<CanvasGroup>();
            Assert.That(waiting.alpha, Is.EqualTo(1f));
            Assert.That(waiting.interactable, Is.True);
            Assert.That(waiting.blocksRaycasts, Is.True);

            Text status = FindText("ArenaStatusLabel");
            Text hitPoints = FindText("MonsterHitPointsLabel");
            GameObject monsterHud = hitPoints.transform.parent.gameObject;
            Assert.That(monsterHud.name, Is.EqualTo("ArenaMonsterHealthPanel"));
            Image fill = Object.FindObjectsByType<Image>(
                    FindObjectsInactive.Include,
                    FindObjectsSortMode.None)
                .Single(image => image.name == "MonsterHealthFill");
            Text terminals = FindText("TerminalCopyLabel");
            Text lootCount = FindText("LootCountLabel");
            Text hint = FindText("InputHintLabel");

            ArenaPlayerProjection[] players = Enumerable.Range(1, 10)
                .Select(id => new ArenaPlayerProjection(
                    (ulong)id,
                    (id - 5) * 900,
                    (id % 2 == 0 ? 1 : -1) * 2500))
                .ToArray();
            view.Render(new ArenaPresentationSnapshot(
                false,
                true,
                true,
                false,
                players,
                new ArenaMonsterProjection(true, 1, 1250, 2000, "Alive", ""),
                Array.Empty<ArenaDropProjection>(),
                "Attack: —",
                "Loot: —",
                remainingCombatSeconds: 27));
            yield return null;

            Assert.That(waiting.alpha, Is.EqualTo(0f));
            Assert.That(waiting.interactable, Is.False);
            Assert.That(waiting.blocksRaycasts, Is.False);
            Assert.That(status.text, Is.EqualTo("전투 진행 중"));
            Assert.That(monsterHud.activeSelf, Is.True);
            Assert.That(hitPoints.text, Is.EqualTo("몬스터\n1250 / 2000 · 00:27"));
            Assert.That(fill.fillAmount, Is.EqualTo(0.625f).Within(0.001f));
            Assert.That(fill.rectTransform.anchorMax.x,
                Is.EqualTo(0.625f).Within(0.001f));
            Assert.That(fill.gameObject.activeSelf, Is.True);
            Assert.That(terminals.text, Is.EqualTo("Attack: —\nLoot: —"));
            Assert.That(lootCount.text, Is.EqualTo("전리품 0"));
            Assert.That(hint.gameObject.activeInHierarchy, Is.True);
            Assert.That(hint.text, Is.EqualTo("이동"));
            Assert.That(FindText("AttackHintLabel").text, Is.EqualTo("공격"));
            Assert.That(FindText("ClaimHintLabel").text, Is.EqualTo("획득"));
            // 키캡은 라벨의 형제라, 힌트가 숨을 때 캡도 같이 숨어야 한다.
            Assert.That(
                FindImage("KeyCapSpace").gameObject.activeInHierarchy,
                Is.True);

            view.Render(new ArenaPresentationSnapshot(
                false,
                true,
                true,
                false,
                players,
                new ArenaMonsterProjection(true, 1, 0, 0, "Alive", ""),
                Array.Empty<ArenaDropProjection>(),
                "Attack: —",
                "Loot: —"));
            yield return null;

            Assert.That(fill.fillAmount, Is.EqualTo(0f));
            Assert.That(fill.gameObject.activeSelf, Is.False);
            Assert.That(monsterHud.activeSelf, Is.True);

            view.Render(new ArenaPresentationSnapshot(
                false,
                true,
                false,
                false,
                players,
                new ArenaMonsterProjection(false, 0, 0, 0, "", ""),
                new[]
                {
                    new ArenaDropProjection(3, 2, 1, 1000, 1500, "Available", 0)
                },
                "Attack: —",
                "Loot: —"));
            yield return null;

            Assert.That(status.text, Is.EqualTo("전투 상태 동기화 중"));
            Assert.That(lootCount.text, Is.EqualTo("전리품 0"));
            Assert.That(monsterHud.activeSelf, Is.False);

            view.Render(new ArenaPresentationSnapshot(
                false,
                true,
                false,
                true,
                players,
                new ArenaMonsterProjection(true, 1, 0, 2000, "Dead", "Defeated"),
                new[]
                {
                    new ArenaDropProjection(3, 2, 1, 1000, 1500, "Available", 0),
                    new ArenaDropProjection(4, 2, 1, -1000, 1500, "Claimed", 1)
                },
                "Attack: Defeated",
                "Loot: —",
                remainingLootSeconds: 12));
            yield return null;

            Assert.That(status.text, Is.EqualTo("전리품 획득 가능"));
            Assert.That(monsterHud.activeSelf, Is.True);
            Assert.That(hitPoints.text, Is.EqualTo("전리품 마감\n00:12"));
            Assert.That(fill.fillAmount, Is.EqualTo(0f));

            // 이 단계에서 패널은 몬스터 체력이 아니라 전리품 마감을 표시한다. fill 만 숨기면
            // Kenney 트랙이 의미 없는 빈 바로 남는다. 패널은 켜진 채여야 카운트다운이 보인다.
            Assert.That(fill.transform.parent.parent.gameObject.name,
                Is.EqualTo("MonsterHealthTrack"));
            Assert.That(fill.transform.parent.parent.gameObject.activeSelf, Is.False,
                "몬스터가 죽으면 체력 트랙도 숨어야 한다");
            Assert.That(Object.FindFirstObjectByType<ArenaWorldView>()
                .transform.Find("Monster-1"), Is.Null);
            Assert.That(terminals.text, Is.EqualTo("Attack: Defeated\nLoot: —"));
            Assert.That(lootCount.text, Is.EqualTo("전리품 1"));

            view.Render(new ArenaPresentationSnapshot(
                true,
                false,
                false,
                false,
                players,
                new ArenaMonsterProjection(false, 0, 0, 0, "", ""),
                Array.Empty<ArenaDropProjection>(),
                "Attack: —",
                "Loot: —"));
            yield return null;

            Assert.That(waiting.alpha, Is.EqualTo(1f));
            Assert.That(waiting.interactable, Is.True);
            Assert.That(waiting.blocksRaycasts, Is.True);
            Assert.That(hint.gameObject.activeInHierarchy, Is.False);
            Assert.That(
                FindImage("KeyCapSpace").gameObject.activeInHierarchy,
                Is.False);
        }

        [UnityTest]
        public IEnumerator MovementMonsterDropAndTerminalCopyComeOnlyFromServerModels()
        {
            var load = new BattleLoadReadModel();
            load.Apply(new ArenaLoadEntry(7, 9));
            load.Apply(GameplayStart());
            var movement = new BattleMovementReadModel(9);
            movement.Apply(new RudpStateSnapshot(
                9,
                2,
                60,
                new[] { new RudpSnapshotPlayer(1, 1250, -1750) }));
            var combat = new BattleCombatReadModel(9);
            combat.Apply(new RudpMonsterSpawned(
                new RudpEventId(1, 1),
                9,
                RudpEventStreamKind.CombatLifecycle,
                1,
                1,
                0,
                0,
                1600,
                4));
            combat.Apply(new RudpAttackTerminalResult(
                RudpCommandId.Create(),
                9,
                RudpAttackResultCode.OutOfRange,
                1,
                1600,
                4,
                RudpCombatOutcome.None));
            var loot = new BattleLootReadModel(9);
            loot.Apply(new RudpDropStateSnapshot(
                9,
                1,
                RudpLootResolutionState.Open,
                new[]
                {
                    new RudpLootDropProjection(
                        3, 2, 1, 3000, 4000, RudpLootDropState.Available, 0)
                }));
            loot.Apply(new RudpClaimLootTerminalResult(
                RudpCommandId.Create(),
                9,
                3,
                RudpClaimLootResultCode.AlreadyClaimed));
            var view = new RecordingArenaView();
            var presenter = new ArenaPresenter(
                new ArenaPlayerFlowReadModel(load, movement, combat, loot),
                new ArenaInputFacade(
                    load,
                    combat,
                    loot,
                    (x, y, cancellation) => Task.CompletedTask,
                    (target, cancellation) => Task.CompletedTask,
                    (drop, cancellation) => Task.CompletedTask),
                view);

            presenter.Render();
            yield return null;

            Assert.That(view.Last.Players, Has.Count.EqualTo(1));
            Assert.That(view.Last.Players[0].Nickname, Is.EqualTo("neo"));
            Assert.That(view.Last.Players[0].PositionXMillimeters, Is.EqualTo(1250));
            Assert.That(view.Last.RemainingCombatSeconds, Is.EqualTo(30));
            Assert.That(view.Last.Monster.HitPoints, Is.EqualTo(1600));
            Assert.That(view.Last.Drops, Has.Count.EqualTo(1));
            Assert.That(view.Last.Drops[0].PositionYMillimeters, Is.EqualTo(4000));
            Assert.That(view.Last.AttackTerminalCopy, Is.EqualTo("Attack: OutOfRange"));
            Assert.That(view.Last.LootTerminalCopy, Is.EqualTo("Loot: AlreadyClaimed"));

            combat.Apply(new RudpCombatTerminalEvent(
                new RudpEventId(2, 2),
                9,
                RudpEventStreamKind.CombatLifecycle,
                2,
                RudpCombatOutcome.MonsterDefeated,
                1,
                100,
                4));
            movement.Apply(new RudpStateSnapshot(
                9,
                3,
                160,
                new[] { new RudpSnapshotPlayer(1, 1250, -1750) }));
            presenter.Render();
            yield return null;

            Assert.That(view.Last.RemainingCombatSeconds, Is.Zero);
            Assert.That(view.Last.RemainingLootSeconds, Is.EqualTo(15));
            Assert.That(view.Last.Monster.State, Is.EqualTo("Dead"));
        }

        [UnityTest]
        public IEnumerator CountdownAdvancesWhileMovementTickIsFrozen()
        {
            var load = new BattleLoadReadModel();
            load.Apply(new ArenaLoadEntry(7, 9));
            load.Apply(GameplayStart());
            var movement = new BattleMovementReadModel(9);
            var combat = new BattleCombatReadModel(9);
            combat.Apply(new RudpMonsterSpawned(
                new RudpEventId(1, 1),
                9,
                RudpEventStreamKind.CombatLifecycle,
                1,
                1,
                0,
                0,
                1600,
                4));
            var flow = new ArenaPlayerFlowReadModel(
                load,
                movement,
                combat,
                new BattleLootReadModel(9));

            Assert.That(flow.Snapshot().RemainingCombatSeconds, Is.EqualTo(30));
            yield return new WaitForSeconds(1.1f);

            Assert.That(flow.Snapshot().RemainingCombatSeconds, Is.LessThan(30));
        }

        private static ArenaGameplayStart GameplayStart()
        {
            return new ArenaGameplayStart(
                7,
                9,
                new[]
                {
                    new BattleParticipant(1, 2, "neo"),
                    new BattleParticipant(2, 3, "trinity")
                });
        }

        private static Text FindText(string name)
        {
            return Object.FindObjectsByType<Text>(
                    FindObjectsInactive.Include,
                    FindObjectsSortMode.None)
                .Single(label => label.name == name);
        }

        private static Image FindImage(string name)
        {
            return Object.FindObjectsByType<Image>(
                    FindObjectsInactive.Include,
                    FindObjectsSortMode.None)
                .Single(image => image.name == name);
        }

        private static ArenaWorldView CreateAppearanceWorld(
            ArenaPlayerAppearanceSet appearances,
            out GameObject owner,
            out GameObject playerPrefab)
        {
            owner = new GameObject("ArenaAppearanceWorldTest");
            playerPrefab = new GameObject("PlayerPrefab");
            playerPrefab.AddComponent<SpriteRenderer>();
            playerPrefab.AddComponent<ArenaPlayerVisual>();
            ArenaWorldView view = owner.AddComponent<ArenaWorldView>();
            SetPrefab(view, "playerPrefab", playerPrefab);
            SetAppearance(view, appearances);
            return view;
        }

        private static ArenaPresentationSnapshot PlayerSnapshot(
            params ArenaPlayerProjection[] players)
        {
            return new ArenaPresentationSnapshot(
                false,
                true,
                false,
                false,
                players,
                new ArenaMonsterProjection(false, 0, 0, 0, string.Empty, string.Empty),
                new ArenaDropProjection[0],
                "Attack: —",
                "Loot: —");
        }

        private static void SetPrefab(
            ArenaWorldView view,
            string fieldName,
            GameObject prefab)
        {
            typeof(ArenaWorldView).GetField(
                fieldName,
                BindingFlags.Instance | BindingFlags.NonPublic).SetValue(view, prefab);
        }

        private static void SetAppearance(
            ArenaWorldView view,
            ArenaPlayerAppearanceSet appearances)
        {
            FieldInfo field = typeof(ArenaWorldView).GetField(
                "appearanceSet",
                BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null);
            field.SetValue(view, appearances);
        }

        private static ArenaPlayerAppearanceSet CreateTestAppearances(
            ICollection<Object> owned)
        {
            ArenaPlayerAppearanceSet appearances =
                ScriptableObject.CreateInstance<ArenaPlayerAppearanceSet>();
            owned.Add(appearances);
            var frames = new Sprite[160];
            for (int slot = 0; slot < ArenaPlayerAppearanceSet.AppearanceCount; slot++)
            {
                var texture = new Texture2D(16, 16);
                texture.name = "Appearance-" + slot;
                owned.Add(texture);
                for (int index = 0; index < 16; index++)
                {
                    Sprite sprite = Sprite.Create(
                        texture,
                        new Rect(0f, 0f, 16f, 16f),
                        new Vector2(0.5f, 0f),
                        16f);
                    sprite.name = $"Appearance-{slot}-Frame-{index}";
                    frames[slot * 16 + index] = sprite;
                    owned.Add(sprite);
                }
            }
            typeof(ArenaPlayerAppearanceSet).GetField(
                "frames",
                BindingFlags.Instance | BindingFlags.NonPublic).SetValue(
                appearances,
                frames);
            return appearances;
        }

        private static void DestroyOwned(IEnumerable<Object> owned)
        {
            foreach (Object item in owned)
            {
                Object.DestroyImmediate(item);
            }
        }

        private sealed class RecordingArenaView : IArenaView
        {
            public ArenaPresentationSnapshot Last { get; private set; }
            public string LastInputCopy { get; private set; }

            public void Render(ArenaPresentationSnapshot snapshot)
            {
                Last = snapshot;
            }

            public void ShowInputAccepted(string copy)
            {
                LastInputCopy = copy;
            }
        }
    }
}
