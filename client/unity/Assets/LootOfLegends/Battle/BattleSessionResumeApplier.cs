using System;
using LootOfLegends.Protocol;
using LootOfLegends.Session;

namespace LootOfLegends.Battle
{
    public sealed class BattleSessionResumeApplier
    {
        private readonly BattleLoadReadModel load;
        private readonly BattleResultReadModel result;
        private readonly ArenaClientRuntime arena;
        private readonly PlayerSessionReadModel session;

        public BattleSessionResumeApplier(
            BattleLoadReadModel load,
            BattleResultReadModel result,
            ArenaClientRuntime arena,
            PlayerSessionReadModel session)
        {
            this.load = load ?? throw new ArgumentNullException(nameof(load));
            this.result = result ?? throw new ArgumentNullException(nameof(result));
            this.arena = arena ?? throw new ArgumentNullException(nameof(arena));
            this.session = session ?? throw new ArgumentNullException(nameof(session));
        }

        public bool Apply(BattleResumeSnapshot snapshot)
        {
            if (snapshot == null)
            {
                throw new ArgumentNullException(nameof(snapshot));
            }
            if (session.State != PlayerSessionState.Authenticated ||
                snapshot.PlayerSessionId != session.SessionId ||
                snapshot.PlayerSessionId != arena.SessionId ||
                snapshot.SessionGeneration != arena.SessionGeneration ||
                snapshot.SessionGeneration != session.SessionGeneration ||
                (load.RoomId != 0 && load.RoomId != snapshot.RoomId) ||
                (load.BattleInstanceId != 0 &&
                 load.BattleInstanceId != snapshot.BattleInstanceId))
            {
                return false;
            }

            // Decode/validation happened at the TCP boundary. Apply all read models on
            // the main thread before the reconnect client sends SnapshotApplied.
            if (!arena.ApplyResumeSnapshot(snapshot) ||
                !result.ApplyResumeSnapshot(snapshot) ||
                !load.ApplyResumeSnapshot(snapshot))
            {
                return false;
            }
            return true;
        }
    }
}
