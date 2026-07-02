using System;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    [DisallowMultipleComponent]
    public sealed class GameSessionRoot : MonoBehaviour
    {
        public const string GenericDisconnectMessage = "서버 연결이 종료되었습니다";
        public const string SessionReplacedDisconnectMessage =
            "다른 클라이언트에서 접속되어 연결이 종료되었습니다";

        public static GameSessionRoot Instance { get; private set; }

        [SerializeField]
        private string gameSessionToken = string.Empty;

        [SerializeField]
        private long reservationExpiresAt;

        [SerializeField]
        private string lastDisconnectMessage = string.Empty;

        private bool hasAdmission;
        private PlayerServerEndpoint gameServerEndpoint;

        public bool HasAdmission => hasAdmission;
        public string GameSessionToken => gameSessionToken;
        public PlayerServerEndpoint GameServerEndpoint => gameServerEndpoint;
        public long ReservationExpiresAt => reservationExpiresAt;
        public string LastDisconnectMessage => lastDisconnectMessage;

        private void Awake()
        {
            if (Instance != null && Instance != this)
            {
                enabled = false;
                DestroyDuplicateComponent();
                return;
            }

            Instance = this;
            if (Application.isPlaying)
            {
                DontDestroyOnLoad(gameObject);
            }
        }

        private void OnDestroy()
        {
            if (Instance == this)
            {
                Instance = null;
            }
        }

        public void StoreAdmission(MetaAdmissionResult admission)
        {
            if (!admission.IsValid)
            {
                throw new ArgumentException(
                    "Admission result must include a token, endpoint, and reservation deadline.",
                    nameof(admission));
            }

            gameSessionToken = admission.GameSessionToken;
            gameServerEndpoint = admission.GameServerEndpoint;
            reservationExpiresAt = admission.ReservationExpiresAt;
            hasAdmission = true;
            lastDisconnectMessage = string.Empty;
        }

        public void ClearAdmission()
        {
            gameSessionToken = string.Empty;
            gameServerEndpoint = default;
            reservationExpiresAt = 0L;
            hasAdmission = false;
        }

        public void MarkDisconnected()
        {
            lastDisconnectMessage = GenericDisconnectMessage;
        }

        public void MarkSessionReplaced()
        {
            lastDisconnectMessage = SessionReplacedDisconnectMessage;
        }

        private void DestroyDuplicateComponent()
        {
            if (Application.isPlaying)
            {
                Destroy(this);
            }
            else
            {
                DestroyImmediate(this);
            }
        }
    }
}
