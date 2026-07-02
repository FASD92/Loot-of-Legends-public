using System;

namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerClientListSnapshot
    {
        private readonly ulong[] sessionIds;

        public PlayerClientListSnapshot(ulong[] sessionIds)
        {
            if (sessionIds == null || sessionIds.Length == 0)
            {
                this.sessionIds = Array.Empty<ulong>();
                return;
            }

            this.sessionIds = new ulong[sessionIds.Length];
            Array.Copy(sessionIds, this.sessionIds, sessionIds.Length);
        }

        public int Count => sessionIds != null ? sessionIds.Length : 0;

        public bool ContainsSession(ulong sessionId)
        {
            if (sessionId == 0UL)
            {
                return false;
            }

            for (int index = 0; index < Count; ++index)
            {
                if (sessionIds[index] == sessionId)
                {
                    return true;
                }
            }

            return false;
        }

        public ulong SessionIdAt(int index)
        {
            if (index < 0 || index >= Count)
            {
                throw new ArgumentOutOfRangeException(nameof(index));
            }

            return sessionIds[index];
        }

        public ulong[] ToArray()
        {
            if (Count == 0)
            {
                return Array.Empty<ulong>();
            }

            ulong[] copy = new ulong[Count];
            Array.Copy(sessionIds, copy, Count);
            return copy;
        }
    }
}
