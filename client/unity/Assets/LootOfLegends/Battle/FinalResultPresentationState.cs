using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace LootOfLegends.Battle
{
    public enum FinalResultPresentationOutcome
    {
        MonsterDefeated,
        CombatTimeout,
        CancelledNoActiveParticipants
    }

    public enum FinalResultPresentationExitStatus
    {
        TerminalPresent,
        TerminalExited
    }

    public sealed class FinalResultPresentationRow
    {
        public FinalResultPresentationRow(
            ulong sessionId,
            string nickname,
            FinalResultPresentationExitStatus exitStatus,
            ulong finalAssetValue,
            uint? rank,
            bool isTop)
        {
            SessionId = sessionId;
            Nickname = nickname ?? throw new ArgumentNullException(nameof(nickname));
            ExitStatus = exitStatus;
            FinalAssetValue = finalAssetValue;
            Rank = rank;
            IsTop = isTop;
        }

        public ulong SessionId { get; }
        public string Nickname { get; }
        public FinalResultPresentationExitStatus ExitStatus { get; }
        public ulong FinalAssetValue { get; }
        public uint? Rank { get; }
        public bool IsTop { get; }
    }

    public sealed class FinalResultPresentationSnapshot
    {
        private readonly IReadOnlyList<FinalResultPresentationRow> rows;

        public FinalResultPresentationSnapshot(
            ulong roomId,
            ulong battleInstanceId,
            FinalResultPresentationOutcome outcome,
            IReadOnlyList<FinalResultPresentationRow> rows)
        {
            RoomId = roomId;
            BattleInstanceId = battleInstanceId;
            Outcome = outcome;
            this.rows = new ReadOnlyCollection<FinalResultPresentationRow>(
                new List<FinalResultPresentationRow>(rows ??
                    throw new ArgumentNullException(nameof(rows))));
        }

        public ulong RoomId { get; }
        public ulong BattleInstanceId { get; }
        public FinalResultPresentationOutcome Outcome { get; }
        public IReadOnlyList<FinalResultPresentationRow> Rows => rows;
        public bool HasWinner => Outcome == FinalResultPresentationOutcome.MonsterDefeated;
    }
}
