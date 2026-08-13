using System;
using LootOfLegends.Battle;

namespace LootOfLegends.Presentation.Common
{
    public interface IBattleRecoveryView
    {
        void ShowBlockingMessage(string copy);
        void HideBlockingMessage();
    }

    public interface IBattleRecoveryLobbyNavigation
    {
        void ReturnToLobby();
    }

    public sealed class BattleRecoveryPresenter
    {
        private const string ResultFailureCopy =
            "결과를 안전하게 확정하지 못했습니다.";
        private const string SettlementPendingCopy =
            "결과를 안전하게 저장 중입니다. 잠시 기다려 주세요.";

        private readonly BattleResultReadModel readModel;
        private readonly IBattleRecoveryView view;
        private readonly IBattleRecoveryLobbyNavigation navigation;
        private BattleRecoveryState presentedState;
        private bool returnedToLobby;

        public BattleRecoveryPresenter(
            BattleResultReadModel readModel,
            IBattleRecoveryView view,
            IBattleRecoveryLobbyNavigation navigation)
        {
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
            this.navigation = navigation ?? throw new ArgumentNullException(nameof(navigation));
        }

        public void Render()
        {
            if (readModel.RecoveryState == BattleRecoveryState.None)
            {
                if (presentedState != BattleRecoveryState.None)
                {
                    view.HideBlockingMessage();
                }
                presentedState = BattleRecoveryState.None;
                returnedToLobby = false;
                return;
            }

            if (presentedState != readModel.RecoveryState)
            {
                view.ShowBlockingMessage(
                    readModel.RecoveryState ==
                        BattleRecoveryState.ResultGenerationFailed
                        ? ResultFailureCopy
                        : SettlementPendingCopy);
                presentedState = readModel.RecoveryState;
            }
            if (readModel.RecoveryState ==
                    BattleRecoveryState.ResultGenerationFailed &&
                readModel.IsLobbyReturnConfirmed && !returnedToLobby)
            {
                returnedToLobby = true;
                navigation.ReturnToLobby();
            }
        }
    }
}
