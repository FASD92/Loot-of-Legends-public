using System;
using System.Threading;
using System.Threading.Tasks;

namespace LootOfLegends.PlayerClient
{
    public interface IPlayerRudpSender : IDisposable
    {
        Task<PlayerRudpHelloSendResult> SendHelloAsync(
            PlayerServerEndpoint endpoint,
            PlayerRudpHello hello,
            CancellationToken cancellationToken);

        Task<PlayerRudpInputCommandSendResult> SendAttackAsync(
            PlayerServerEndpoint endpoint,
            ulong sessionId,
            uint targetHintMonsterId,
            CancellationToken cancellationToken);

        Task<PlayerRudpInputCommandSendResult> SendMoveAsync(
            PlayerServerEndpoint endpoint,
            ulong sessionId,
            short dirX,
            short dirY,
            CancellationToken cancellationToken);

        Task<PlayerRudpInputCommandSendResult> SendSpaceLootAsync(
            PlayerServerEndpoint endpoint,
            ulong sessionId,
            CancellationToken cancellationToken);

        bool TryReceiveStateSnapshot(out PlayerRudpStateSnapshot snapshot);

        void Disconnect();
    }
}
