using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;

namespace LootOfLegends.Presentation.Arena
{
    public interface IArenaView
    {
        void Render(ArenaPresentationSnapshot snapshot);
        void ShowInputAccepted(string copy);
    }

    public sealed class ArenaPresenter
    {
        private readonly ArenaPlayerFlowReadModel readModel;
        private readonly ArenaInputFacade input;
        private readonly IArenaView view;

        public ArenaPresenter(
            ArenaPlayerFlowReadModel readModel,
            ArenaInputFacade input,
            IArenaView view)
        {
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
            this.input = input ?? throw new ArgumentNullException(nameof(input));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
        }

        public void Render()
        {
            view.Render(readModel.Snapshot());
        }

        public async Task MoveAsync(
            short desiredX,
            short desiredY,
            CancellationToken cancellationToken)
        {
            await input.MoveAsync(desiredX, desiredY, cancellationToken);
            view.ShowInputAccepted("Move submitted — waiting for server snapshot");
        }

        public async Task AttackAsync(ulong targetId, CancellationToken cancellationToken)
        {
            await input.AttackAsync(targetId, cancellationToken);
            view.ShowInputAccepted("Attack submitted — waiting for terminal result");
        }

        public async Task ClaimAsync(ulong dropId, CancellationToken cancellationToken)
        {
            await input.ClaimAsync(dropId, cancellationToken);
            view.ShowInputAccepted("Loot submitted — waiting for terminal result");
        }
    }
}
