using System;
using LootOfLegends.Session;

namespace LootOfLegends.Presentation.Common
{
    public interface ISafeFailureView
    {
        void ShowBlockingMessage(string copy);
    }

    public interface ILoginNavigation
    {
        void ReturnToLogin();
    }

    public sealed class SafeFailurePresenter : IDisposable
    {
        private const string SessionReplacedCopy =
            "다른 로그인으로 현재 세션이 종료되었습니다.";
        private readonly PlayerSessionReadModel session;
        private readonly ISafeFailureView view;
        private readonly ILoginNavigation navigation;
        private bool begun;
        private PlayerSessionFailure handledFailure;

        public SafeFailurePresenter(
            PlayerSessionReadModel session,
            ISafeFailureView view,
            ILoginNavigation navigation)
        {
            this.session = session ?? throw new ArgumentNullException(nameof(session));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
            this.navigation = navigation ?? throw new ArgumentNullException(nameof(navigation));
        }

        public void Begin()
        {
            if (begun)
            {
                throw new InvalidOperationException("Safe failure presenter is already active");
            }
            begun = true;
            session.Changed += Render;
            Render();
        }

        public void Dispose()
        {
            if (!begun)
            {
                return;
            }
            begun = false;
            session.Changed -= Render;
        }

        private void Render()
        {
            if (session.LastFailure == PlayerSessionFailure.None)
            {
                handledFailure = PlayerSessionFailure.None;
                return;
            }
            if (session.LastFailure == handledFailure)
            {
                return;
            }
            handledFailure = session.LastFailure;
            view.ShowBlockingMessage(
                session.LastFailure == PlayerSessionFailure.SessionReplaced
                    ? SessionReplacedCopy
                    : "게임 연결을 유지하지 못해 로그인 화면으로 돌아갑니다.");
            navigation.ReturnToLogin();
        }
    }
}
