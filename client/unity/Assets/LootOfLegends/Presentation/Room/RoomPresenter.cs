using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.LobbyRoom;

namespace LootOfLegends.Presentation.Room
{
    public interface IRoomHostStartAction
    {
        Task<RoomCommandResult> StartAsync(CancellationToken cancellationToken);
    }

    public interface IRoomView
    {
        void Render(RoomPresentationSnapshot snapshot);
        void ShowCommandResult(RoomCommandResult result);
    }

    public sealed class RoomPresenter : IDisposable
    {
        private readonly LobbyRoomReadModel readModel;
        private readonly ILobbyRoomCommands commands;
        private readonly IRoomHostStartAction hostStart;
        private readonly IRoomView view;
        private bool begun;

        public RoomPresenter(
            LobbyRoomReadModel readModel,
            ILobbyRoomCommands commands,
            IRoomHostStartAction hostStart,
            IRoomView view)
        {
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
            this.commands = commands ?? throw new ArgumentNullException(nameof(commands));
            this.hostStart = hostStart ?? throw new ArgumentNullException(nameof(hostStart));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
        }

        public void Begin()
        {
            if (begun)
            {
                throw new InvalidOperationException("Room presenter is already active");
            }
            begun = true;
            readModel.Changed += Render;
            Render();
        }

        public Task SetReadyAsync(bool ready, CancellationToken cancellationToken)
        {
            return ExecuteAsync(commands.SetReadyAsync(ready, cancellationToken));
        }

        public Task HostStartAsync(CancellationToken cancellationToken)
        {
            return ExecuteAsync(hostStart.StartAsync(cancellationToken));
        }

        public Task LeaveAsync(CancellationToken cancellationToken)
        {
            return ExecuteAsync(commands.LeaveAsync(cancellationToken));
        }

        public Task KickAsync(
            ulong targetSessionId,
            ulong targetSessionGeneration,
            CancellationToken cancellationToken)
        {
            return ExecuteAsync(commands.KickAsync(
                targetSessionId,
                targetSessionGeneration,
                cancellationToken));
        }

        public void Dispose()
        {
            if (!begun)
            {
                return;
            }
            begun = false;
            readModel.Changed -= Render;
        }

        private async Task ExecuteAsync(Task<RoomCommandResult> command)
        {
            RoomCommandResult result = await command;
            if (begun)
            {
                view.ShowCommandResult(result);
            }
        }

        private void Render()
        {
            view.Render(readModel.Room);
        }
    }

}
