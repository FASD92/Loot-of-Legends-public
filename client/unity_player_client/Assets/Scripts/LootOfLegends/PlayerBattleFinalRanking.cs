using System;

namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerBattleFinalRankingRow
    {
        public PlayerBattleFinalRankingRow(
            ushort rank,
            ulong sessionId,
            string nickname,
            long totalAssetValue)
        {
            Rank = rank;
            SessionId = sessionId;
            Nickname = nickname ?? string.Empty;
            TotalAssetValue = totalAssetValue;
        }

        public ushort Rank { get; }

        public ulong SessionId { get; }

        public string Nickname { get; }

        public long TotalAssetValue { get; }
    }

    public readonly struct PlayerBattleFinalRanking
    {
        private readonly PlayerBattleFinalRankingRow[] rows;

        public PlayerBattleFinalRanking(
            uint roomId,
            ulong battleInstanceId,
            PlayerBattleFinalRankingRow[] rows)
        {
            RoomId = roomId;
            BattleInstanceId = battleInstanceId;

            if (rows == null || rows.Length == 0)
            {
                this.rows = Array.Empty<PlayerBattleFinalRankingRow>();
                return;
            }

            this.rows = new PlayerBattleFinalRankingRow[rows.Length];
            Array.Copy(rows, this.rows, rows.Length);
        }

        public uint RoomId { get; }

        public ulong BattleInstanceId { get; }

        public int Count => rows != null ? rows.Length : 0;

        public PlayerBattleFinalRankingRow RowAt(int index)
        {
            if (index < 0 || index >= Count)
            {
                throw new ArgumentOutOfRangeException(nameof(index));
            }

            return rows[index];
        }

        public PlayerBattleFinalRankingRow[] ToArray()
        {
            if (Count == 0)
            {
                return Array.Empty<PlayerBattleFinalRankingRow>();
            }

            PlayerBattleFinalRankingRow[] copy = new PlayerBattleFinalRankingRow[Count];
            Array.Copy(rows, copy, Count);
            return copy;
        }
    }
}
