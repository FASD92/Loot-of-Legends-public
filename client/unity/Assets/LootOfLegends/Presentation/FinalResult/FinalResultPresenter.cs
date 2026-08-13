using System;
using LootOfLegends.Battle;

namespace LootOfLegends.Presentation.FinalResult
{
    public interface IFinalResultView
    {
        void Show(FinalResultPresentationSnapshot snapshot);
        void Hide();
    }

    public interface IRoomReturnNavigation
    {
        void ReturnToRoom(ulong roomId);
    }

    public sealed class FinalResultPresenter
    {
        private readonly BattleResultReadModel readModel;
        private readonly IFinalResultView view;
        private readonly IRoomReturnNavigation navigation;
        private ulong presentedBattleInstanceId;
        private ulong returnedBattleInstanceId;

        public FinalResultPresenter(
            BattleResultReadModel readModel,
            IFinalResultView view,
            IRoomReturnNavigation navigation)
        {
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
            this.navigation = navigation ?? throw new ArgumentNullException(nameof(navigation));
        }

        public void Render()
        {
            FinalResultPresentationSnapshot snapshot = readModel.Snapshot();
            if (snapshot == null)
            {
                if (presentedBattleInstanceId != 0)
                {
                    view.Hide();
                }
                presentedBattleInstanceId = 0;
                returnedBattleInstanceId = 0;
                return;
            }

            if (presentedBattleInstanceId != snapshot.BattleInstanceId)
            {
                view.Show(snapshot);
                presentedBattleInstanceId = snapshot.BattleInstanceId;
            }

            if (readModel.IsReadyForRematch &&
                returnedBattleInstanceId != snapshot.BattleInstanceId)
            {
                navigation.ReturnToRoom(snapshot.RoomId);
                returnedBattleInstanceId = snapshot.BattleInstanceId;
            }
        }
    }
}
