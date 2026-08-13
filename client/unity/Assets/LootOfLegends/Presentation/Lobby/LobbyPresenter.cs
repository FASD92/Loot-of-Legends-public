using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.LobbyRoom;

namespace LootOfLegends.Presentation.Lobby
{
    public interface ILobbyView
    {
        void Render(LobbyPresentationSnapshot snapshot);
        void ShowCommandResult(RoomCommandResult result);
    }

    public sealed class LobbyPresenter : IDisposable
    {
        private readonly LobbyRoomReadModel readModel;
        private readonly ILobbyRoomCommands commands;
        private readonly ILobbyView view;
        private bool begun;

        public LobbyPresenter(
            LobbyRoomReadModel readModel,
            ILobbyRoomCommands commands,
            ILobbyView view)
        {
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
            this.commands = commands ?? throw new ArgumentNullException(nameof(commands));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
        }

        public void Begin()
        {
            if (begun)
            {
                throw new InvalidOperationException("Lobby presenter is already active");
            }
            begun = true;
            readModel.Changed += Render;
            Render();
        }

        public Task CreateAsync(
            string title,
            byte capacity,
            CancellationToken cancellationToken)
        {
            return ExecuteAsync(
                commands.CreateAsync(title, capacity, cancellationToken));
        }

        public Task JoinAsync(ulong roomId, CancellationToken cancellationToken)
        {
            return ExecuteAsync(commands.JoinAsync(roomId, cancellationToken));
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
            view.Render(readModel.Lobby);
        }
    }

}
