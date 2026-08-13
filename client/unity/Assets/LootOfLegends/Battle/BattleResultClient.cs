using System;
using System.Collections.Generic;
using System.Linq;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;

namespace LootOfLegends.Battle
{
    public enum BattleRecoveryState
    {
        None,
        ResultGenerationFailed,
        SettlementRecoveryPending
    }

    public sealed class BattleResultReadModel
    {
        public ulong CurrentRoomId { get; private set; }
        public ulong CurrentBattleInstanceId { get; private set; }
        public BattleFinalResult FinalResult { get; private set; }
        public bool HasFinalResult => FinalResult != null;
        public bool IsReadyForRematch { get; private set; }
        public BattleRecoveryState RecoveryState { get; private set; }
        public bool IsLobbyReturnConfirmed { get; private set; }
        public bool IsBattleDisposed { get; private set; }

        public void BeginBattle(ulong roomId, ulong battleInstanceId)
        {
            if (roomId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(roomId));
            }
            if (battleInstanceId == 0)
            {
                throw new ArgumentOutOfRangeException(nameof(battleInstanceId));
            }
            CurrentRoomId = roomId;
            CurrentBattleInstanceId = battleInstanceId;
            FinalResult = null;
            IsReadyForRematch = false;
            RecoveryState = BattleRecoveryState.None;
            IsLobbyReturnConfirmed = false;
            IsBattleDisposed = false;
        }

        public bool Apply(BattleRecoveryNotice notice)
        {
            if (notice == null)
            {
                throw new ArgumentNullException(nameof(notice));
            }
            if (IsBattleDisposed || RecoveryState != BattleRecoveryState.None ||
                HasFinalResult || notice.RoomId != CurrentRoomId ||
                notice.BattleInstanceId != CurrentBattleInstanceId)
            {
                return false;
            }
            RecoveryState = notice.Reason == BattleRecoveryReason.ResultGenerationFailed
                ? BattleRecoveryState.ResultGenerationFailed
                : BattleRecoveryState.SettlementRecoveryPending;
            return true;
        }

        public bool Apply(BattleFinalResult result)
        {
            if (result == null)
            {
                throw new ArgumentNullException(nameof(result));
            }
            if (IsBattleDisposed || HasFinalResult ||
                RecoveryState == BattleRecoveryState.ResultGenerationFailed ||
                result.RoomId != CurrentRoomId ||
                result.BattleInstanceId != CurrentBattleInstanceId)
            {
                return false;
            }
            FinalResult = result;
            RecoveryState = BattleRecoveryState.None;
            IsReadyForRematch = false;
            return true;
        }

        public bool ConfirmLobbyReturn()
        {
            if (RecoveryState != BattleRecoveryState.ResultGenerationFailed ||
                IsLobbyReturnConfirmed)
            {
                return false;
            }
            IsLobbyReturnConfirmed = true;
            IsBattleDisposed = true;
            CurrentRoomId = 0;
            CurrentBattleInstanceId = 0;
            FinalResult = null;
            IsReadyForRematch = false;
            return true;
        }

        public bool Apply(RoomDetailProjection room)
        {
            if (room == null)
            {
                throw new ArgumentNullException(nameof(room));
            }
            if (!HasFinalResult || IsReadyForRematch || room.RoomId != CurrentRoomId ||
                room.Members.Any(member => member.Ready))
            {
                return false;
            }
            IsReadyForRematch = true;
            return true;
        }

        public FinalResultPresentationSnapshot Snapshot()
        {
            if (!HasFinalResult)
            {
                return null;
            }

            var rows = new List<FinalResultPresentationRow>(FinalResult.Entries.Count);
            foreach (FinalResultEntry entry in FinalResult.Entries)
            {
                rows.Add(new FinalResultPresentationRow(
                    entry.SessionId,
                    entry.Nickname,
                    entry.ExitStatus == FinalResultExitStatus.TerminalPresent
                        ? FinalResultPresentationExitStatus.TerminalPresent
                        : FinalResultPresentationExitStatus.TerminalExited,
                    entry.FinalAssetValue,
                    entry.Rank,
                    entry.IsTop));
            }
            return new FinalResultPresentationSnapshot(
                FinalResult.RoomId,
                FinalResult.BattleInstanceId,
                MapOutcome(FinalResult.Outcome),
                rows);
        }

        private static FinalResultPresentationOutcome MapOutcome(FinalResultOutcome outcome)
        {
            switch (outcome)
            {
                case FinalResultOutcome.MonsterDefeated:
                    return FinalResultPresentationOutcome.MonsterDefeated;
                case FinalResultOutcome.CombatTimeout:
                    return FinalResultPresentationOutcome.CombatTimeout;
                default:
                    return FinalResultPresentationOutcome.CancelledNoActiveParticipants;
            }
        }
    }

    public sealed class BattleCompletionRouter :
        ITcpInboundMessageSink,
        ILobbyRoomInboundMessageSink,
        IFinalResultInboundMessageSink,
        IBattleRecoveryInboundMessageSink
    {
        private readonly BattleLoadMessageRouter loadRouter;
        private readonly BattleLoadReadModel load;
        private readonly BattleResultReadModel completion;

        public BattleCompletionRouter(
            BattleResponseCorrelator correlator,
            BattleLoadReadModel load,
            BattleResultReadModel completion)
        {
            this.load = load ?? throw new ArgumentNullException(nameof(load));
            loadRouter = new BattleLoadMessageRouter(
                correlator ?? throw new ArgumentNullException(nameof(correlator)),
                this.load);
            this.completion = completion ?? throw new ArgumentNullException(nameof(completion));
        }

        public void OnMessage(BattleLoadServerMessage message)
        {
            loadRouter.OnMessage(message);
            if (message is ArenaLoadEntry entry)
            {
                completion.BeginBattle(entry.RoomId, entry.BattleInstanceId);
            }
        }

        public void OnMessage(LobbyRoomServerMessage message)
        {
            if (message is LobbyEntrySnapshot && completion.ConfirmLobbyReturn())
            {
                load.ResetForLobby();
                return;
            }
            if (message is RoomDetailProjection room)
            {
                completion.Apply(room);
            }
        }

        public void OnMessage(BattleFinalResult message)
        {
            if (completion.Apply(message))
            {
                load.ApplyFinalResult(message.RoomId, message.BattleInstanceId);
            }
        }

        public void OnMessage(BattleRecoveryNotice message)
        {
            if (completion.Apply(message))
            {
                load.Apply(message);
            }
        }
    }
}
