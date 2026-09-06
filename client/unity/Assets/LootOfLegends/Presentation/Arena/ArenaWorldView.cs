using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using LootOfLegends.Battle;
using LootOfLegends.Presentation.Common;
using UnityEngine;

namespace LootOfLegends.Presentation.Arena
{
    public sealed class ArenaWorldView : MonoBehaviour
    {
        private static readonly Color MonsterHitColor =
            new Color(1f, 0.4f, 0.15f, 1f);
        private const float MonsterHitSeconds = 0.12f;

        [SerializeField] private PresentationCatalog catalog;
        [SerializeField] private AudioSource audioSource;
        [SerializeField] private GameObject playerPrefab;
        [SerializeField] private GameObject monsterPrefab;
        [SerializeField] private GameObject dropPrefab;

        private readonly Dictionary<ulong, GameObject> players =
            new Dictionary<ulong, GameObject>();
        private readonly Dictionary<ulong, GameObject> drops =
            new Dictionary<ulong, GameObject>();
        private readonly Dictionary<ulong, int> appearanceSlots =
            new Dictionary<ulong, int>();
        private ArenaPlayerAppearanceSet appearanceSet;
        private bool rosterInitialized;
        private GameObject monster;
        private Color monsterBaseColor = Color.white;
        private uint monsterHitGeneration;
        private uint lastAttackSequence;
        private ulong localSessionId;
        private ulong hostSessionId;

        public void SetPlayerContext(ulong localId, ulong hostId)
        {
            localSessionId = localId;
            hostSessionId = hostId;
        }

        public void Render(ArenaPresentationSnapshot snapshot)
        {
            if (snapshot == null)
            {
                throw new ArgumentNullException(nameof(snapshot));
            }

            SyncPlayers(snapshot.Players);
            bool removesMonster = !snapshot.Monster.HasMonster ||
                snapshot.Monster.State == "Dead";
            if (!removesMonster)
            {
                SyncMonster(snapshot.Monster);
            }
            SyncAttack(snapshot.LastAppliedAttack);
            if (removesMonster)
            {
                SyncMonster(snapshot.Monster);
            }
            SyncDrops(snapshot.Drops);
        }

        private void SyncPlayers(IReadOnlyList<ArenaPlayerProjection> next)
        {
            EnsureAppearanceSlots(next);
            foreach (ulong stale in players.Keys
                         .Where(id => next.All(player => player.SessionId != id))
                         .ToArray())
            {
                Remove(players, stale);
            }

            foreach (ArenaPlayerProjection projection in next)
            {
                bool created = !players.ContainsKey(projection.SessionId);
                GameObject player = GetOrCreate(
                    players,
                    projection.SessionId,
                    playerPrefab,
                    "Player-");
                ArenaPlayerVisual visual = player.GetComponent<ArenaPlayerVisual>();
                if (visual == null)
                {
                    throw new InvalidOperationException(
                        "Arena player prefab requires ArenaPlayerVisual");
                }
                if (created)
                {
                    visual.Bind(Appearances, appearanceSlots[projection.SessionId]);
                }
                string marker = projection.SessionId == localSessionId &&
                    projection.SessionId == hostSessionId
                        ? " (나·방장)"
                        : projection.SessionId == localSessionId
                            ? " (나)"
                            : projection.SessionId == hostSessionId
                                ? " (방장)"
                                : string.Empty;
                visual.SetNickname(projection.Nickname + marker);
                visual.Project(WorldPosition(
                    projection.PositionXMillimeters,
                    projection.PositionYMillimeters));
            }
        }

        private ArenaPlayerAppearanceSet Appearances => appearanceSet ??=
            (catalog != null
                ? catalog.Resolve<ArenaPlayerAppearanceSet>(
                    "character.player.appearance-set")
                : throw new InvalidOperationException(
                    "Arena presentation catalog is not bound"));

        private void EnsureAppearanceSlots(IReadOnlyList<ArenaPlayerProjection> next)
        {
            if (!rosterInitialized)
            {
                if (next.Count == 0)
                {
                    return;
                }
                if (next.Count > ArenaPlayerAppearanceSet.AppearanceCount)
                {
                    throw new InvalidOperationException(
                        "Arena roster exceeds the ten appearance slots");
                }

                int slot = 0;
                foreach (ArenaPlayerProjection player in next.OrderBy(
                             projection => projection.SessionId))
                {
                    if (appearanceSlots.ContainsKey(player.SessionId))
                    {
                        throw new InvalidOperationException(
                            "Arena roster contains a duplicate SessionId");
                    }
                    appearanceSlots.Add(player.SessionId, slot++);
                }
                rosterInitialized = true;
                return;
            }

            foreach (ArenaPlayerProjection player in next)
            {
                if (!appearanceSlots.ContainsKey(player.SessionId))
                {
                    throw new InvalidOperationException(
                        "Arena snapshot contains a player outside the initial roster");
                }
            }
        }

        private void SyncMonster(ArenaMonsterProjection next)
        {
            if (!next.HasMonster || next.State == "Dead")
            {
                if (monster != null)
                {
                    PlaySound("audio.sfx.monster-death");
                    monsterHitGeneration++;
                    monster.SetActive(false);
                    Destroy(monster);
                    monster = null;
                }
                return;
            }

            if (monster == null)
            {
                monster = Instantiate(monsterPrefab, transform, false);
                SpriteRenderer renderer = monster.GetComponent<SpriteRenderer>();
                monsterBaseColor = renderer != null ? renderer.color : Color.white;
            }
            monster.name = "Monster-" + next.MonsterId;
            monster.transform.localPosition = Vector3.zero;
            monster.SetActive(true);
        }

        private void SyncDrops(IReadOnlyList<ArenaDropProjection> next)
        {
            bool claimed = drops.Keys.Any(id => next.Any(drop =>
                drop.DropId == id && drop.State == "Claimed"));
            foreach (ulong stale in drops.Keys
                         .Where(id => next.All(drop =>
                             drop.DropId != id || drop.State != "Available"))
                         .ToArray())
            {
                Remove(drops, stale);
            }

            foreach (ArenaDropProjection projection in next.Where(
                         drop => drop.State == "Available"))
            {
                GameObject drop = GetOrCreate(
                    drops,
                    projection.DropId,
                    dropPrefab,
                    "Drop-");
                drop.GetComponent<ArenaDropVisual>().Project(projection.ItemId);
                drop.transform.localPosition = WorldPosition(
                    projection.PositionXMillimeters,
                    projection.PositionYMillimeters);
            }
            if (claimed)
            {
                PlaySound("audio.sfx.loot-pickup");
            }
        }

        private void SyncAttack(ArenaAttackProjection attack)
        {
            if (attack == null || attack.Sequence <= lastAttackSequence ||
                !players.TryGetValue(attack.AttackerSessionId, out GameObject attacker) ||
                monster == null)
            {
                return;
            }
            lastAttackSequence = attack.Sequence;
            PlaySound("audio.sfx.attack");
            var effect = new GameObject("AttackEffect-" + attack.Sequence);
            effect.transform.SetParent(transform, false);
            effect.AddComponent<LineRenderer>();
            effect.AddComponent<ArenaAttackProjectile>().Begin(
                attacker.transform.position,
                monster.transform.position,
                OnAttackImpact);
        }

        private void OnAttackImpact()
        {
            PlaySound("audio.sfx.hit");
            FlashMonster();
        }

        private void FlashMonster()
        {
            if (monster == null)
            {
                return;
            }
            SpriteRenderer renderer = monster.GetComponent<SpriteRenderer>();
            if (renderer == null)
            {
                return;
            }
            uint generation = ++monsterHitGeneration;
            renderer.color = MonsterHitColor;
            StartCoroutine(RestoreMonsterColor(renderer, generation));
        }

        private IEnumerator RestoreMonsterColor(
            SpriteRenderer renderer,
            uint generation)
        {
            yield return new WaitForSeconds(MonsterHitSeconds);
            if (renderer != null && generation == monsterHitGeneration)
            {
                renderer.color = monsterBaseColor;
            }
        }

        private GameObject GetOrCreate(
            IDictionary<ulong, GameObject> objects,
            ulong id,
            GameObject prefab,
            string namePrefix)
        {
            if (!objects.TryGetValue(id, out GameObject instance))
            {
                instance = Instantiate(prefab, transform, false);
                instance.name = namePrefix + id;
                instance.SetActive(true);
                objects.Add(id, instance);
            }
            return instance;
        }

        private static void Remove(IDictionary<ulong, GameObject> objects, ulong id)
        {
            GameObject instance = objects[id];
            objects.Remove(id);
            if (instance != null)
            {
                instance.SetActive(false);
                Destroy(instance);
            }
        }

        private static Vector3 WorldPosition(int xMillimeters, int yMillimeters)
        {
            return new Vector3(xMillimeters / 1000f, yMillimeters / 1000f, 0f);
        }

        private void PlaySound(string key)
        {
            if (catalog != null && audioSource != null)
            {
                audioSource.PlayOneShot(catalog.Resolve<AudioClip>(key));
            }
        }
    }
}
