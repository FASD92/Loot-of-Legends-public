using System;
using System.Threading.Tasks;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public interface IStandaloneMetaSessionClient
    {
        Task<MetaHttpResult<MetaAuthSession>> ExchangeStandaloneCodeAsync(
            string metaBaseUrl,
            string code,
            string state);
    }

    public interface IMetaSessionClient : IStandaloneMetaSessionClient
    {
        Task<MetaHttpResult<MetaNicknameAvailability>> CheckNicknameAsync(
            string metaBaseUrl,
            string nickname);

        Task<MetaHttpResult<MetaAuthSession>> UpdateNicknameAsync(
            string metaBaseUrl,
            string nickname);

        Task<MetaHttpResult<MetaAdmissionOutcome>> EnterAdmissionAsync(
            string metaBaseUrl,
            string inviteCode);

        Task<MetaHttpResult<MetaAdmissionOutcome>> GetQueueStatusAsync(
            string metaBaseUrl,
            string queueToken);
    }

    public interface IStandaloneOAuthLoopbackSession : IDisposable
    {
        string CallbackUri { get; }
        Task<StandaloneOAuthCallback> WaitForCallbackAsync(TimeSpan timeout);
    }

    public interface IStandaloneOAuthLoopbackFactory
    {
        IStandaloneOAuthLoopbackSession Start(string expectedState);
    }

    public interface IExternalUrlOpener
    {
        void OpenUrl(string url);
    }

    public sealed class StandaloneOAuthLoginFlow : IStandaloneOAuthLoginFlow
    {
        public static readonly TimeSpan DefaultCallbackTimeout =
            TimeSpan.FromMinutes(2.0);

        private readonly IStandaloneOAuthLoopbackFactory loopbackFactory;
        private readonly IExternalUrlOpener externalUrlOpener;
        private readonly IStandaloneMetaSessionClient metaSessionClient;
        private readonly TimeSpan callbackTimeout;

        public StandaloneOAuthLoginFlow()
            : this(
                new StandaloneOAuthLoopbackFactory(),
                new UnityExternalUrlOpener(),
                new MetaHttpSessionClient(),
                DefaultCallbackTimeout)
        {
        }

        public StandaloneOAuthLoginFlow(
            IStandaloneOAuthLoopbackFactory loopbackFactory,
            IExternalUrlOpener externalUrlOpener,
            IStandaloneMetaSessionClient metaSessionClient,
            TimeSpan callbackTimeout)
        {
            this.loopbackFactory = loopbackFactory ??
                throw new ArgumentNullException(nameof(loopbackFactory));
            this.externalUrlOpener = externalUrlOpener ??
                throw new ArgumentNullException(nameof(externalUrlOpener));
            this.metaSessionClient = metaSessionClient ??
                throw new ArgumentNullException(nameof(metaSessionClient));
            if (callbackTimeout <= TimeSpan.Zero)
            {
                throw new ArgumentOutOfRangeException(nameof(callbackTimeout));
            }

            this.callbackTimeout = callbackTimeout;
        }

        public async Task<MetaHttpResult<MetaAuthSession>> LoginAsync(string metaBaseUrl)
        {
            try
            {
                string state = StandaloneOAuthClient.CreateState();
                using IStandaloneOAuthLoopbackSession loopback =
                    loopbackFactory.Start(state);
                string startUrl = StandaloneOAuthClient.BuildStartUrl(
                    metaBaseUrl,
                    loopback.CallbackUri,
                    state);
                externalUrlOpener.OpenUrl(startUrl);
                StandaloneOAuthCallback callback =
                    await loopback.WaitForCallbackAsync(callbackTimeout)
                        .ConfigureAwait(false);
                return await metaSessionClient.ExchangeStandaloneCodeAsync(
                        metaBaseUrl,
                        callback.Code,
                        callback.State)
                    .ConfigureAwait(false);
            }
            catch (ArgumentException exception)
            {
                return MetaHttpResult<MetaAuthSession>.Failure(0, exception.Message);
            }
            catch (InvalidOperationException exception)
            {
                return MetaHttpResult<MetaAuthSession>.Failure(0, exception.Message);
            }
            catch (TimeoutException exception)
            {
                return MetaHttpResult<MetaAuthSession>.Failure(0, exception.Message);
            }
        }
    }

    public sealed class StandaloneOAuthLoopbackFactory :
        IStandaloneOAuthLoopbackFactory
    {
        public IStandaloneOAuthLoopbackSession Start(string expectedState)
        {
            return new StandaloneOAuthLoopbackSession(
                StandaloneOAuthLoopbackListener.Start(expectedState));
        }
    }

    public sealed class UnityExternalUrlOpener : IExternalUrlOpener
    {
        public void OpenUrl(string url)
        {
            Application.OpenURL(url);
        }
    }

    public sealed class StandaloneOAuthLoopbackSession :
        IStandaloneOAuthLoopbackSession
    {
        private readonly StandaloneOAuthLoopbackListener listener;

        public StandaloneOAuthLoopbackSession(
            StandaloneOAuthLoopbackListener listener)
        {
            this.listener = listener ??
                throw new ArgumentNullException(nameof(listener));
        }

        public string CallbackUri => listener.CallbackUri;

        public Task<StandaloneOAuthCallback> WaitForCallbackAsync(
            TimeSpan timeout)
        {
            return listener.WaitForCallbackAsync(timeout);
        }

        public void Dispose()
        {
            listener.Dispose();
        }
    }
}
