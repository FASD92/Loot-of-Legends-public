using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Collection;

namespace LootOfLegends.Presentation.Collection
{
    public interface ICollectionView
    {
        void Render(CollectionPresentationSnapshot snapshot);
    }

    public sealed class CollectionPresenter
    {
        private readonly CollectionReadModel readModel;
        private readonly ICollectionApi api;
        private readonly ICollectionView view;

        public CollectionPresenter(
            CollectionReadModel readModel,
            ICollectionApi api,
            ICollectionView view)
        {
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
            this.api = api ?? throw new ArgumentNullException(nameof(api));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
        }

        public async Task RefreshAsync(CancellationToken cancellationToken)
        {
            readModel.BeginRefresh();
            Render();
            try
            {
                CollectionSnapshot snapshot = await api.FetchAsync(cancellationToken);
                readModel.Apply(snapshot);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                readModel.MarkUnavailable();
                Render();
                throw;
            }
            catch (Exception)
            {
                readModel.MarkUnavailable();
            }
            Render();
        }

        public void Render()
        {
            if (view is UnityEngine.Object unityView && unityView == null)
            {
                return;
            }
            view.Render(readModel.Snapshot());
        }
    }
}
