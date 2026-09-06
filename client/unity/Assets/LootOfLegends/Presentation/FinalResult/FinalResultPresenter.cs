using System;
using System.Threading;
using System.Threading.Tasks;
using LootOfLegends.Battle;
using LootOfLegends.LobbyRoom;

namespace LootOfLegends.Presentation.FinalResult
{
    public interface IFinalResultView
    {
        void Show(FinalResultPresentationSnapshot snapshot);
        void SetActions(bool enabled, string statusCopy);
        void Hide();
    }

    public interface IRoomReturnNavigation
    {
        void ReturnToRoom(ulong roomId);
    }

    public sealed class FinalResultPresenter
    {
        private readonly BattleResultReadModel readModel;
        private readonly IFinalResultView view;
        private readonly IRoomReturnNavigation navigation;
        private readonly Func<CancellationToken, Task<RoomCommandResult>> leaveRoom;
        private ulong presentedBattleInstanceId;
        private ulong returnedBattleInstanceId;
        private bool leavePending;
        private bool awaitingLobbyProjection;
        private string actionStatus;

        public FinalResultPresenter(
            BattleResultReadModel readModel,
            IFinalResultView view,
            IRoomReturnNavigation navigation,
            Func<CancellationToken, Task<RoomCommandResult>> leaveRoom)
        {
            this.readModel = readModel ?? throw new ArgumentNullException(nameof(readModel));
            this.view = view ?? throw new ArgumentNullException(nameof(view));
            this.navigation = navigation ?? throw new ArgumentNullException(nameof(navigation));
            this.leaveRoom = leaveRoom ?? throw new ArgumentNullException(nameof(leaveRoom));
        }

        public void Render()
        {
            FinalResultPresentationSnapshot snapshot = readModel.Snapshot();
            if (snapshot == null)
            {
                if (presentedBattleInstanceId != 0)
                {
                    view.Hide();
                }
                presentedBattleInstanceId = 0;
                returnedBattleInstanceId = 0;
                leavePending = false;
                awaitingLobbyProjection = false;
                actionStatus = null;
                return;
            }

            if (presentedBattleInstanceId != snapshot.BattleInstanceId)
            {
                leavePending = false;
                awaitingLobbyProjection = false;
                actionStatus = null;
                view.Show(snapshot);
                presentedBattleInstanceId = snapshot.BattleInstanceId;
            }

            RenderActions();
        }

        public void ReturnToRoom()
        {
            FinalResultPresentationSnapshot snapshot = readModel.Snapshot();
            if (snapshot != null && readModel.IsReadyForRematch &&
                returnedBattleInstanceId != snapshot.BattleInstanceId)
            {
                navigation.ReturnToRoom(snapshot.RoomId);
                returnedBattleInstanceId = snapshot.BattleInstanceId;
            }
        }

        public async Task ReturnToLobbyAsync(
            CancellationToken cancellationToken)
        {
            if (leavePending || awaitingLobbyProjection ||
                !readModel.IsReadyForRematch)
            {
                return;
            }

            leavePending = true;
            actionStatus = "로비로 나가는 중입니다.";
            RenderActions();
            try
            {
                RoomCommandResult result = await leaveRoom(cancellationToken);
                awaitingLobbyProjection = result == RoomCommandResult.Ok;
                actionStatus = awaitingLobbyProjection
                    ? "서버 로비 상태를 기다리고 있습니다."
                    : "로비로 나가지 못했습니다. 다시 시도해 주세요.";
            }
            catch (Exception)
            {
                actionStatus = "로비로 나가지 못했습니다. 다시 시도해 주세요.";
            }
            finally
            {
                leavePending = false;
                RenderActions();
            }
        }

        private void RenderActions()
        {
            bool ready = readModel.IsReadyForRematch;
            view.SetActions(
                ready && !leavePending && !awaitingLobbyProjection,
                actionStatus ?? (ready
                    ? "이동할 곳을 선택하세요."
                    : "경기 종료 처리를 기다리고 있습니다."));
        }
    }
}
