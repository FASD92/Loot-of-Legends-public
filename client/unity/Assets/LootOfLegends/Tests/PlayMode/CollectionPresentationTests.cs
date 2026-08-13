using System;
using System.Collections;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Collection;
using LootOfLegends.Presentation.Collection;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class CollectionPresentationTests
    {
        [UnityTest]
        public IEnumerator RefreshShowsOnlyAppliedQuantityAndSeparatePendingCount()
        {
            var model = new CollectionReadModel();
            model.Apply(Snapshot(quantity: 4, wallet: 300, pending: 1));
            var source = new RecordingCollectionApi(
                Snapshot(quantity: 4, wallet: 300, pending: 2));
            var view = new RecordingCollectionView();
            var presenter = new CollectionPresenter(model, source, view);

            Task refresh = presenter.RefreshAsync(CancellationToken.None);
            yield return new WaitUntil(() => refresh.IsCompleted);

            Assert.That(source.Calls, Is.EqualTo(1));
            Assert.That(view.States, Is.EqualTo(new[]
            {
                CollectionPresentationState.Loading,
                CollectionPresentationState.Fresh
            }));
            Assert.That(view.Last.Items, Has.Count.EqualTo(1));
            Assert.That(view.Last.Items[0].Quantity, Is.EqualTo(4),
                "pending settlement must not predict Applied quantity");
            Assert.That(view.Last.Wallet, Is.EqualTo(300));
            Assert.That(view.Last.PendingSettlementCount, Is.EqualTo(2));
        }

        [UnityTest]
        public IEnumerator FailedRefreshKeepsConfirmedDataAsStaleWithBoundedCopy()
        {
            var model = new CollectionReadModel();
            model.Apply(Snapshot(quantity: 4, wallet: 300, pending: 1));
            var view = new RecordingCollectionView();
            var presenter = new CollectionPresenter(
                model,
                new ThrowingCollectionApi(
                    "Bearer secret-token https://10.0.0.8/internal stack trace"),
                view);

            Task refresh = presenter.RefreshAsync(CancellationToken.None);
            yield return new WaitUntil(() => refresh.IsCompleted);

            Assert.That(refresh.IsFaulted, Is.False);
            Assert.That(view.Last.State, Is.EqualTo(CollectionPresentationState.Stale));
            Assert.That(view.Last.Items[0].Quantity, Is.EqualTo(4));
            Assert.That(view.Last.StatusCopy, Does.Not.Contain("Bearer"));
            Assert.That(view.Last.StatusCopy, Does.Not.Contain("10.0.0.8"));
            Assert.That(view.Last.StatusCopy, Does.Not.Contain("stack"));
            Assert.That(view.Last.StatusCopy.Length, Is.LessThanOrEqualTo(80));
        }

        [UnityTest]
        public IEnumerator InitialFailureShowsErrorWithoutInventingCollection()
        {
            var model = new CollectionReadModel();
            var view = new RecordingCollectionView();
            var presenter = new CollectionPresenter(
                model,
                new ThrowingCollectionApi("raw-internal-error"),
                view);

            Task refresh = presenter.RefreshAsync(CancellationToken.None);
            yield return new WaitUntil(() => refresh.IsCompleted);

            Assert.That(view.Last.State, Is.EqualTo(CollectionPresentationState.Error));
            Assert.That(view.Last.Items, Is.Empty);
            Assert.That(view.Last.Wallet, Is.Zero);
            Assert.That(view.Last.PendingSettlementCount, Is.Zero);
            Assert.That(view.Last.StatusCopy, Does.Not.Contain("raw-internal-error"));
        }

        [UnityTest]
        public IEnumerator RefreshIgnoresAViewDestroyedBySceneTransition()
        {
            var model = new CollectionReadModel();
            var source = new DeferredCollectionApi();
            var root = new GameObject("CollectionView");
            var view = root.AddComponent<CollectionTextView>();
            var presenter = new CollectionPresenter(model, source, view);

            Task refresh = presenter.RefreshAsync(CancellationToken.None);
            UnityEngine.Object.Destroy(root);
            yield return null;
            source.Complete(Snapshot(quantity: 1, wallet: 100, pending: 0));
            yield return new WaitUntil(() => refresh.IsCompleted);

            Assert.That(refresh.IsFaulted, Is.False);
        }

        private static CollectionSnapshot Snapshot(
            ulong quantity,
            ulong wallet,
            long pending)
        {
            return new CollectionSnapshot(
                new[] { new CollectionItem(1, quantity, 100) },
                wallet,
                pending);
        }

        private sealed class RecordingCollectionApi : ICollectionApi
        {
            private readonly CollectionSnapshot snapshot;

            public RecordingCollectionApi(CollectionSnapshot snapshot)
            {
                this.snapshot = snapshot;
            }

            public int Calls { get; private set; }

            public Task<CollectionSnapshot> FetchAsync(CancellationToken cancellationToken)
            {
                Calls++;
                return Task.FromResult(snapshot);
            }
        }

        private sealed class ThrowingCollectionApi : ICollectionApi
        {
            private readonly string message;

            public ThrowingCollectionApi(string message)
            {
                this.message = message;
            }

            public Task<CollectionSnapshot> FetchAsync(CancellationToken cancellationToken)
            {
                throw new InvalidOperationException(message);
            }
        }

        private sealed class DeferredCollectionApi : ICollectionApi
        {
            private readonly TaskCompletionSource<CollectionSnapshot> completion =
                new TaskCompletionSource<CollectionSnapshot>(
                    TaskCreationOptions.RunContinuationsAsynchronously);

            public Task<CollectionSnapshot> FetchAsync(CancellationToken cancellationToken)
            {
                return completion.Task;
            }

            public void Complete(CollectionSnapshot snapshot)
            {
                completion.TrySetResult(snapshot);
            }
        }

        private sealed class RecordingCollectionView : ICollectionView
        {
            public List<CollectionPresentationState> States { get; } =
                new List<CollectionPresentationState>();
            public CollectionPresentationSnapshot Last { get; private set; }

            public void Render(CollectionPresentationSnapshot snapshot)
            {
                Last = snapshot;
                States.Add(snapshot.State);
            }
        }
    }
}
