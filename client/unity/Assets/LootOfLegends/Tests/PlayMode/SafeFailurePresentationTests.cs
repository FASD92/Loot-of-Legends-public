using System.Collections;
using LootOfLegends.Battle;
using LootOfLegends.Presentation.Common;
using LootOfLegends.Protocol;
using LootOfLegends.Session;
using NUnit.Framework;
using UnityEngine.TestTools;

namespace LootOfLegends.Tests.PlayMode
{
    public sealed class SafeFailurePresentationTests
    {
        [UnityTest]
        public IEnumerator SessionReplacedShowsBoundedCopyAndReturnsLoginOnce()
        {
            var session = new PlayerSessionReadModel();
            session.BeginAuthentication();
            session.Apply(new WelcomeSession(1, 2, 3, 1700000000000, "player-one"));
            var view = new RecordingFailureView();
            var navigation = new RecordingLoginNavigation();
            using (var presenter = new SafeFailurePresenter(session, view, navigation))
            {
                presenter.Begin();
                session.Apply(new SessionReplaced(SessionReplacedReason.SameAccountLogin));
                session.Apply(new SessionReplaced(SessionReplacedReason.SameAccountLogin));
                yield return null;
            }

            Assert.That(view.Calls, Is.EqualTo(1));
            Assert.That(view.Copy,
                Is.EqualTo("다른 로그인으로 현재 세션이 종료되었습니다."));
            Assert.That(view.Copy.Length, Is.LessThanOrEqualTo(80));
            Assert.That(navigation.Calls, Is.EqualTo(1));
        }

        [UnityTest]
        public IEnumerator ConfirmedRudpFailureEndsSessionWithoutTcpFallback()
        {
            var session = new PlayerSessionReadModel();
            session.BeginAuthentication();
            session.Apply(new WelcomeSession(1, 2, 3, 1700000000000, "player-one"));
            var view = new RecordingFailureView();
            var navigation = new RecordingLoginNavigation();
            using (var presenter = new SafeFailurePresenter(session, view, navigation))
            {
                presenter.Begin();
                Assert.That(session.ConfirmRudpFailure(), Is.True);
                Assert.That(session.ConfirmRudpFailure(), Is.False);
                yield return null;
            }

            Assert.That(session.State, Is.EqualTo(PlayerSessionState.Disconnected));
            Assert.That(session.LastFailure, Is.EqualTo(PlayerSessionFailure.RudpUnavailable));
            Assert.That(view.Calls, Is.EqualTo(1));
            Assert.That(view.Copy,
                Is.EqualTo("게임 연결을 유지하지 못해 로그인 화면으로 돌아갑니다."));
            Assert.That(navigation.Calls, Is.EqualTo(1));
        }

        [UnityTest]
        public IEnumerator ServerLoadMinimumFailureReturnsRoomOnce()
        {
            var load = new BattleLoadReadModel();
            var view = new RecordingFailureView();
            var navigation = new RecordingRoomNavigation();
            using (var presenter = new BattleLoadFailurePresenter(load, view, navigation))
            {
                presenter.Begin();
                load.Apply(new ArenaLoadEntry(7, 9));
                load.Apply(new ArenaLoadCancelled(
                    7, 9, ArenaLoadCancelReason.NotEnoughReady));
                load.Apply(new ArenaLoadCancelled(
                    7, 9, ArenaLoadCancelReason.NotEnoughReady));
                yield return null;
            }

            Assert.That(view.Calls, Is.EqualTo(1));
            Assert.That(view.Copy, Is.EqualTo("플레이 인원이 부족해 방으로 돌아갑니다."));
            Assert.That(navigation.Calls, Is.EqualTo(1));
        }

        private sealed class RecordingFailureView : ISafeFailureView
        {
            public int Calls { get; private set; }
            public string Copy { get; private set; }

            public void ShowBlockingMessage(string copy)
            {
                Calls++;
                Copy = copy;
            }
        }

        private sealed class RecordingLoginNavigation : ILoginNavigation
        {
            public int Calls { get; private set; }

            public void ReturnToLogin()
            {
                Calls++;
            }
        }

        private sealed class RecordingRoomNavigation : IRoomNavigation
        {
            public int Calls { get; private set; }

            public void ReturnToRoom()
            {
                Calls++;
            }
        }
    }
}
