using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using LootOfLegends.Protocol;

namespace LootOfLegends.Battle
{
    public sealed class BattlePlayerStateView
    {
        internal BattlePlayerStateView(BattleResumePlayerState state)
        {
            SessionId = state.SessionId;
            HealthKnown = state.HealthKnown;
            HitPoints = state.HitPoints;
            MaximumHitPoints = state.MaximumHitPoints;
            IsAlive = state.IsAlive;
        }

        public ulong SessionId { get; }
        public bool HealthKnown { get; }
        public uint HitPoints { get; }
        public uint MaximumHitPoints { get; }
        public bool IsAlive { get; }
    }

    public sealed class BattlePlayerStateReadModel
    {
        private readonly Dictionary<ulong, BattlePlayerStateView> players =
            new Dictionary<ulong, BattlePlayerStateView>();
        private readonly IReadOnlyDictionary<ulong, BattlePlayerStateView> readOnlyPlayers;

        public BattlePlayerStateReadModel()
        {
            readOnlyPlayers = new ReadOnlyDictionary<ulong, BattlePlayerStateView>(players);
        }

        public IReadOnlyDictionary<ulong, BattlePlayerStateView> Players => readOnlyPlayers;

        public bool ApplyResumeSnapshot(BattleResumeSnapshot snapshot)
        {
            if (snapshot == null)
            {
                throw new ArgumentNullException(nameof(snapshot));
            }
            players.Clear();
            foreach (BattleResumePlayerState player in snapshot.Players)
            {
                players.Add(player.SessionId, new BattlePlayerStateView(player));
            }
            return true;
        }
    }
}
