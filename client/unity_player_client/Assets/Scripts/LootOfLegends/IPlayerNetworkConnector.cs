using System;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public interface IPlayerNetworkConnector : IDisposable
    {
        bool HasPendingPacket { get; }

        Task<PlayerNetworkConnectResult> ConnectAsync(
            PlayerServerEndpoint endpoint,
            string gameSessionToken,
            CancellationToken cancellationToken);

        Task SendPacketAsync(byte[] packet, CancellationToken cancellationToken);

        Task<byte[]> ReceivePacketAsync(CancellationToken cancellationToken);

        void Disconnect();
    }
}
