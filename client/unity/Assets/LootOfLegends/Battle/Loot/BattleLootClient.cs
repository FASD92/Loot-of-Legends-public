using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Transport.Rudp;

namespace LootOfLegends.Battle.Loot
{
    public sealed class ClaimLootInputFacade
    {
        private readonly IRudpDatagramSender sender;
        private readonly Func<RudpHeader> nextHeader;
        private readonly RudpReliableOutbound reliableOutbound;
        private readonly ulong battleInstanceId;

        public ClaimLootInputFacade(
            IRudpDatagramSender sender,
            Func<RudpHeader> nextHeader,
            ulong battleInstanceId)
        {
            this.sender = sender ?? throw new ArgumentNullException(nameof(sender));
            this.nextHeader = nextHeader ?? throw new ArgumentNullException(nameof(nextHeader));
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            this.battleInstanceId = battleInstanceId;
        }

        public ClaimLootInputFacade(
            RudpReliableOutbound reliableOutbound,
            ulong battleInstanceId)
        {
            this.reliableOutbound = reliableOutbound ??
                throw new ArgumentNullException(nameof(reliableOutbound));
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            this.battleInstanceId = battleInstanceId;
        }

        public async Task<RudpCommandId> ClaimAsync(
            ulong dropId,
            CancellationToken cancellationToken)
        {
            if (dropId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(dropId));
            }
            if (reliableOutbound != null)
            {
                return await reliableOutbound.SendClaimLootAsync(
                        battleInstanceId,
                        dropId,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            RudpCommandId commandId = RudpCommandId.Create();
            byte[] datagram = RudpProtocolCodec.EncodeClaimLootIntent(
                nextHeader(), commandId, battleInstanceId, dropId);
            await sender.SendAsync(datagram, cancellationToken).ConfigureAwait(false);
            return commandId;
        }
    }

    public sealed class BattleLootDropView
    {
        internal BattleLootDropView(RudpLootDropProjection projection)
        {
            DropId = projection.DropId;
            ItemId = projection.ItemId;
            Quantity = projection.Quantity;
            PositionXMillimeters = projection.PositionXMillimeters;
            PositionYMillimeters = projection.PositionYMillimeters;
            State = projection.State;
            OwnerSessionId = projection.OwnerSessionId;
        }

        internal BattleLootDropView(RudpDropSpawned spawned)
        {
            DropId = spawned.DropId;
            ItemId = spawned.ItemId;
            Quantity = spawned.Quantity;
            PositionXMillimeters = spawned.PositionXMillimeters;
            PositionYMillimeters = spawned.PositionYMillimeters;
            State = RudpLootDropState.Available;
        }

        public ulong DropId { get; }
        public ulong ItemId { get; }
        public ulong Quantity { get; }
        public int PositionXMillimeters { get; }
        public int PositionYMillimeters { get; }
        public RudpLootDropState State { get; }
        public bool IsAvailable => State == RudpLootDropState.Available;
        public string StateName => State.ToString();
        public ulong OwnerSessionId { get; }

        internal bool SameIdentity(RudpDropSpawned spawned)
        {
            return DropId == spawned.DropId && ItemId == spawned.ItemId &&
                   Quantity == spawned.Quantity &&
                   PositionXMillimeters == spawned.PositionXMillimeters &&
                   PositionYMillimeters == spawned.PositionYMillimeters;
        }
    }

    public sealed class BattleLootReadModel
    {
        private readonly ulong battleInstanceId;
        private readonly int ownerThreadId;
        private IReadOnlyList<BattleLootDropView> drops =
            new ReadOnlyCollection<BattleLootDropView>(new List<BattleLootDropView>());
        private bool hasSnapshot;

        public BattleLootReadModel(ulong battleInstanceId)
        {
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            this.battleInstanceId = battleInstanceId;
            ownerThreadId = Thread.CurrentThread.ManagedThreadId;
        }

        public IReadOnlyList<BattleLootDropView> Drops => drops;
        public uint SnapshotSequence { get; private set; }
        public RudpLootResolutionState ResolutionState { get; private set; }
        public RudpCommandId LastClaimCommandId { get; private set; }
        public RudpClaimLootResultCode? LastClaimResult { get; private set; }

        public bool Apply(object serverMessage)
        {
            if (Thread.CurrentThread.ManagedThreadId != ownerThreadId)
            {
                throw new InvalidOperationException(
                    "Loot read model must update on its owner thread");
            }
            if (serverMessage == null)
            {
                throw new ArgumentNullException(nameof(serverMessage));
            }
            switch (serverMessage)
            {
                case RudpDropSpawned spawned:
                    return Apply(spawned);
                case RudpClaimLootTerminalResult result:
                    return Apply(result);
                case RudpDropStateSnapshot snapshot:
                    return Apply(snapshot);
                default:
                    return false;
            }
        }

        private bool Apply(RudpDropSpawned spawned)
        {
            if (spawned.BattleInstanceId != battleInstanceId)
            {
                return false;
            }
            BattleLootDropView existing = drops.FirstOrDefault(
                drop => drop.DropId == spawned.DropId);
            if (existing != null)
            {
                return existing.SameIdentity(spawned);
            }
            var next = new List<BattleLootDropView>(drops)
            {
                new BattleLootDropView(spawned)
            };
            next.Sort((left, right) => left.DropId.CompareTo(right.DropId));
            drops = new ReadOnlyCollection<BattleLootDropView>(next);
            return true;
        }

        private bool Apply(RudpClaimLootTerminalResult result)
        {
            if (result.BattleInstanceId != battleInstanceId)
            {
                return false;
            }
            LastClaimCommandId = result.CommandId;
            LastClaimResult = result.ResultCode;
            return true;
        }

        private bool Apply(RudpDropStateSnapshot snapshot)
        {
            if (snapshot.BattleInstanceId != battleInstanceId ||
                (hasSnapshot && !RudpProtocolCodec.IsSequenceNewer(
                    snapshot.SnapshotSequence, SnapshotSequence)))
            {
                return false;
            }
            drops = new ReadOnlyCollection<BattleLootDropView>(
                snapshot.Drops
                    .OrderBy(drop => drop.DropId)
                    .Select(drop => new BattleLootDropView(drop))
                    .ToList());
            SnapshotSequence = snapshot.SnapshotSequence;
            ResolutionState = snapshot.ResolutionState;
            hasSnapshot = true;
            return true;
        }
    }
}
