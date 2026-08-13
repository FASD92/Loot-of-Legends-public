using System;
using LootOfLegends.Battle;

namespace LootOfLegends.Presentation.Common
{
    public interface IRoomNavigation
    {
        void ReturnToRoom();
    }

    public sealed class BattleLoadFailurePresenter : IDisposable
    {
        private const string MinimumFailureCopy =
            "플레이 인원이 부족해 방으로 돌아갑니다.";
        private readonly BattleLoadReadModel load;
        private readonly ISafeFailureView view;
        private readonly IRoomNavigation navigation;
        private bool begun;
        private ulong handledBattleInstanceId;

        public BattleLoadFailurePresenter(
            BattleLoadReadModel load,
            ISafeFailureView view,
            IRoomNavigation navigation)
        {
            this.load = load ?? throw new ArgumentNullException(nameof(load));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
            this.navigation = navigation ?? throw new ArgumentNullException(nameof(navigation));
        }

        public void Begin()
        {
            if (begun)
            {
                throw new InvalidOperationException(
                    "Battle load failure presenter is already active");
            }
            begun = true;
            load.Changed += Render;
            Render();
        }

        public void Dispose()
        {
            if (!begun)
            {
                return;
            }
            begun = false;
            load.Changed -= Render;
        }

        private void Render()
        {
            if (!load.HasLoadFailure ||
                load.BattleInstanceId == handledBattleInstanceId)
            {
                return;
            }
            handledBattleInstanceId = load.BattleInstanceId;
            view.ShowBlockingMessage(MinimumFailureCopy);
            navigation.ReturnToRoom();
        }
    }
}
