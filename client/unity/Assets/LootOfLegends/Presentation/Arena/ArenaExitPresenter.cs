using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.LobbyRoom;

namespace LootOfLegends.Presentation.Arena
{
    public interface IArenaExitView
    {
        void ShowExitStatus(string copy);
    }

    public interface ILobbyNavigation
    {
        void ReturnToLobby();
    }

    public sealed class ArenaExitPresenter : IDisposable
    {
        private readonly LobbyRoomReadModel readModel;
        private readonly ILobbyRoomCommands commands;
        private readonly IArenaExitView view;
        private readonly ILobbyNavigation navigation;
        private bool begun;
        private bool awaitingLobbyProjection;
        private bool navigated;

        public ArenaExitPresenter(
            LobbyRoomReadModel readModel,
            ILobbyRoomCommands commands,
            IArenaExitView view,
            ILobbyNavigation navigation)
        {
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
            this.commands = commands ?? throw new ArgumentNullException(nameof(commands));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
            this.navigation = navigation ?? throw new ArgumentNullException(nameof(navigation));
        }

        public void Begin()
        {
            if (begun)
            {
                throw new InvalidOperationException("Arena exit presenter is already active");
            }
            begun = true;
            readModel.Changed += Render;
        }

        public async Task LeaveAsync(CancellationToken cancellationToken)
        {
            RoomCommandResult result = await commands.LeaveAsync(cancellationToken);
            if (!begun)
            {
                return;
            }
            if (result != RoomCommandResult.Ok)
            {
                view.ShowExitStatus("전투에서 나가지 못했습니다. 다시 시도해 주세요.");
                return;
            }
            awaitingLobbyProjection = true;
            Render();
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

        private void Render()
        {
            if (!begun || !awaitingLobbyProjection || navigated ||
                readModel.IsInRoom)
            {
                return;
            }
            navigated = true;
            view.ShowExitStatus("전투에서 나와 로비로 돌아갑니다.");
            navigation.ReturnToLobby();
        }
    }
}
