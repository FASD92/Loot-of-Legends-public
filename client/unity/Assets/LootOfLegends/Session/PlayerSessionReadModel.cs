using System;
using LootOfLegends.Protocol;
using LootOfLegends.Transport;

namespace LootOfLegends.Session
{
    public enum PlayerSessionState
    {
        Disconnected,
        Authenticating,
        Authenticated,
        Replaced,
        Closing
    }

    public enum PlayerSessionFailure
    {
        None,
        SessionReplaced,
        RudpUnavailable
    }

    public sealed class PlayerSessionReadModel : ISessionInboundMessageSink
    {
        public event Action Changed;

        public PlayerSessionState State { get; private set; } =
            PlayerSessionState.Disconnected;
        public ulong SessionId { get; private set; }
        public ulong SessionGeneration { get; private set; }
        public string Nickname { get; private set; } = string.Empty;
        public PlayerSessionFailure LastFailure { get; private set; }

        public void BeginAuthentication()
        {
            SessionId = 0;
            SessionGeneration = 0;
            Nickname = string.Empty;
            LastFailure = PlayerSessionFailure.None;
            SetState(PlayerSessionState.Authenticating);
        }

        public bool Apply(SessionServerMessage message)
        {
            if (message == null)
            {
                throw new ArgumentNullException(nameof(message));
            }
            switch (message)
            {
                case WelcomeSession welcome
                    when State == PlayerSessionState.Authenticating:
                    SessionId = welcome.SessionId;
                    SessionGeneration = welcome.SessionGeneration;
                    Nickname = welcome.Nickname;
                    SetState(PlayerSessionState.Authenticated);
                    return true;
                case AuthenticationRejectedSession _
                    when State == PlayerSessionState.Authenticating:
                    SessionId = 0;
                    SessionGeneration = 0;
                    Nickname = string.Empty;
                    SetState(PlayerSessionState.Disconnected);
                    return true;
                case SessionReplaced _
                    when State == PlayerSessionState.Authenticated:
                    SessionId = 0;
                    SessionGeneration = 0;
                    Nickname = string.Empty;
                    LastFailure = PlayerSessionFailure.SessionReplaced;
                    SetState(PlayerSessionState.Replaced);
                    return true;
                default:
                    return false;
            }
        }

        public bool ConfirmRudpFailure()
        {
            if (State != PlayerSessionState.Authenticated ||
                LastFailure != PlayerSessionFailure.None)
            {
                return false;
            }
            SessionId = 0;
            SessionGeneration = 0;
            Nickname = string.Empty;
            LastFailure = PlayerSessionFailure.RudpUnavailable;
            SetState(PlayerSessionState.Disconnected);
            return true;
        }

        public void BeginClosing()
        {
            if (State != PlayerSessionState.Disconnected)
            {
                SetState(PlayerSessionState.Closing);
            }
        }

        public void Disconnect()
        {
            SessionId = 0;
            SessionGeneration = 0;
            Nickname = string.Empty;
            LastFailure = PlayerSessionFailure.None;
            SetState(PlayerSessionState.Disconnected);
        }

        void ISessionInboundMessageSink.OnMessage(SessionServerMessage message)
        {
            Apply(message);
        }

        private void SetState(PlayerSessionState state)
        {
            if (State == state)
            {
                return;
            }
            State = state;
            Changed?.Invoke();
        }
    }
}
