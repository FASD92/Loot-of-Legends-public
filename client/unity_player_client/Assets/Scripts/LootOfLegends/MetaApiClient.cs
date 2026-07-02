using System;
using UnityEngine;

namespace LootOfLegends.PlayerClient
{
    public enum MetaQueueState
    {
        Queued,
        Admitted,
        GameServerReadinessFailed,
        QueueExpired,
        NotQueued
    }

    public enum MetaNicknameValidationFailure
    {
        Missing,
        Length,
        TooLong,
        UnsupportedCharacters,
        Duplicate,
        AlreadySet,
        Unknown
    }

    public readonly struct MetaQueueStatus
    {
        public readonly MetaQueueState State;
        public readonly int Position;
        public readonly string QueueToken;

        public MetaQueueStatus(MetaQueueState state, int position)
            : this(state, position, string.Empty)
        {
        }

        public MetaQueueStatus(MetaQueueState state, int position, string queueToken)
        {
            State = state;
            Position = Math.Max(0, position);
            QueueToken = queueToken ?? string.Empty;
        }
    }

    public readonly struct MetaAdmissionResult
    {
        public readonly string GameSessionToken;
        public readonly PlayerServerEndpoint GameServerEndpoint;
        public readonly long ReservationExpiresAt;

        public MetaAdmissionResult(string gameSessionToken, PlayerServerEndpoint gameServerEndpoint)
            : this(gameSessionToken, gameServerEndpoint, 0L)
        {
        }

        public MetaAdmissionResult(
            string gameSessionToken,
            PlayerServerEndpoint gameServerEndpoint,
            long reservationExpiresAt)
        {
            GameSessionToken = gameSessionToken ?? string.Empty;
            GameServerEndpoint = gameServerEndpoint;
            ReservationExpiresAt = Math.Max(0L, reservationExpiresAt);
        }

        public static MetaAdmissionResult Empty =>
            new MetaAdmissionResult(string.Empty, default, 0L);

        public bool IsValid =>
            !string.IsNullOrWhiteSpace(GameSessionToken) &&
            GameServerEndpoint.IsValid &&
            ReservationExpiresAt > 0L;
    }

    public readonly struct MetaAdmissionOutcome
    {
        public readonly MetaQueueStatus Status;
        public readonly MetaAdmissionResult Admission;

        public MetaAdmissionOutcome(
            MetaQueueStatus status,
            MetaAdmissionResult admission)
        {
            Status = status;
            Admission = admission;
        }
    }

    public readonly struct MetaAuthSession
    {
        public readonly bool Authenticated;
        public readonly long AccountId;
        public readonly bool NicknameRequired;
        public readonly string Nickname;

        public MetaAuthSession(
            bool authenticated,
            long accountId,
            bool nicknameRequired,
            string nickname)
        {
            Authenticated = authenticated;
            AccountId = Math.Max(0L, accountId);
            NicknameRequired = authenticated && nicknameRequired;
            Nickname = nickname ?? string.Empty;
        }

        public static MetaAuthSession Unauthenticated =>
            new MetaAuthSession(false, 0L, false, string.Empty);
    }

    public readonly struct MetaNicknameAvailability
    {
        public readonly bool Available;
        public readonly string Message;

        public MetaNicknameAvailability(bool available, string message)
        {
            Available = available;
            Message = message ?? string.Empty;
        }
    }

    public readonly struct MetaApiRequest
    {
        public readonly string Method;
        public readonly string Path;
        public readonly string BodyJson;

        public MetaApiRequest(string method, string path)
            : this(method, path, string.Empty)
        {
        }

        public MetaApiRequest(string method, string path, string bodyJson)
        {
            Method = method ?? string.Empty;
            Path = path ?? string.Empty;
            BodyJson = bodyJson ?? string.Empty;
        }
    }

    [Serializable]
    public sealed class MetaAuthSessionResponse
    {
        public bool authenticated;
        public long accountId;
        public bool nicknameRequired;
        public string nickname;
    }

    [Serializable]
    public sealed class MetaStandaloneExchangeRequest
    {
        public string code;
        public string state;
    }

    [Serializable]
    public sealed class MetaNicknameRequest
    {
        public string nickname;
    }

    [Serializable]
    public sealed class MetaNicknameResponse
    {
        public bool succeeded;
        public string message;
    }

    [Serializable]
    public sealed class MetaNicknameAvailabilityResponse
    {
        public bool available;
        public string message;
    }

    [Serializable]
    public sealed class MetaAdmissionRequest
    {
        public string inviteCode;
    }

    [Serializable]
    public sealed class MetaAdmissionResponse
    {
        public string state;
        public string status;
        public string queueToken;
        public int position;
        public string gameSessionToken;
        public string gameServerHost;
        public int tcpPort;
        public int rudpPort;
        public long reservationExpiresAt;
        public MetaGameServerEndpointResponse gameServerEndpoint;
    }

    [Serializable]
    public sealed class MetaQueueStatusResponse
    {
        public string state;
        public string status;
        public string queueToken;
        public int position;
        public string gameSessionToken;
        public string gameServerHost;
        public int tcpPort;
        public int rudpPort;
        public long reservationExpiresAt;
        public MetaGameServerEndpointResponse gameServerEndpoint;
    }

    [Serializable]
    public sealed class MetaGameServerEndpointResponse
    {
        public string host;
        public int tcpPort;
        public int rudpPort;
    }

    public sealed class MetaApiClient
    {
        public const string Release0AuthSessionPath = "/api/release0/auth/session";
        public const string Release0AuthCsrfPath = "/api/release0/auth/csrf";
        public const string Release0AuthNicknamePath = "/api/release0/auth/nickname";
        public const string Release0AuthNicknameCheckPath =
            "/api/release0/auth/nickname/check";
        public const string Release0StandaloneExchangePath =
            "/api/release0/auth/standalone/exchange";
        public const string Release0AdmissionPath = "/api/release0/admission/enter";
        public const string Release0AdmissionStatusPath = "/api/release0/admission/status";
        public const string Release0AdmissionQueuePath = "/api/release0/admission/queue";

        public string SessionPath => Release0AuthSessionPath;
        public string CsrfPath => Release0AuthCsrfPath;
        public string NicknamePath => Release0AuthNicknamePath;
        public string NicknameCheckPath => Release0AuthNicknameCheckPath;
        public string StandaloneExchangePath => Release0StandaloneExchangePath;
        public string AdmissionPath => Release0AdmissionPath;
        public string AdmissionStatusPath => Release0AdmissionStatusPath;

        public string QueueStatusPath(string queueToken)
        {
            return Release0AdmissionQueuePath + "/" + EscapeQueueToken(queueToken);
        }

        public string QueueCancelPath()
        {
            return Release0AdmissionQueuePath;
        }

        public string QueueCancelPath(string queueToken)
        {
            return QueueStatusPath(queueToken);
        }

        public MetaApiRequest BuildSessionRequest()
        {
            return new MetaApiRequest("GET", SessionPath);
        }

        public MetaApiRequest BuildCsrfRequest()
        {
            return new MetaApiRequest("GET", CsrfPath);
        }

        public MetaApiRequest BuildStandaloneExchangeRequest(string code, string state)
        {
            return new MetaApiRequest(
                "POST",
                StandaloneExchangePath,
                JsonUtility.ToJson(new MetaStandaloneExchangeRequest
                {
                    code = code ?? string.Empty,
                    state = state ?? string.Empty
                }));
        }

        public MetaApiRequest BuildNicknameRequest(string nickname)
        {
            return new MetaApiRequest(
                "POST",
                NicknamePath,
                JsonUtility.ToJson(new MetaNicknameRequest { nickname = nickname ?? string.Empty }));
        }

        public MetaApiRequest BuildNicknameCheckRequest(string nickname)
        {
            return new MetaApiRequest(
                "POST",
                NicknameCheckPath,
                JsonUtility.ToJson(new MetaNicknameRequest { nickname = nickname ?? string.Empty }));
        }

        public MetaApiRequest BuildAdmissionRequest()
        {
            return BuildAdmissionRequest(string.Empty);
        }

        public MetaApiRequest BuildAdmissionRequest(string inviteCode)
        {
            return new MetaApiRequest(
                "POST",
                AdmissionPath,
                JsonUtility.ToJson(new MetaAdmissionRequest
                {
                    inviteCode = inviteCode ?? string.Empty
                }));
        }

        public MetaApiRequest BuildAdmissionStatusRequest()
        {
            return new MetaApiRequest("GET", AdmissionStatusPath);
        }

        public MetaApiRequest BuildQueueStatusRequest(string queueToken)
        {
            return new MetaApiRequest("GET", QueueStatusPath(queueToken));
        }

        public MetaApiRequest BuildQueueCancelRequest()
        {
            return new MetaApiRequest("DELETE", QueueCancelPath());
        }

        public MetaApiRequest BuildQueueCancelRequest(string queueToken)
        {
            return new MetaApiRequest("DELETE", QueueCancelPath(queueToken));
        }

        public static bool TryParseAuthSessionJson(
            string json,
            out MetaAuthSession session,
            out string error)
        {
            session = MetaAuthSession.Unauthenticated;
            error = string.Empty;
            if (string.IsNullOrWhiteSpace(json))
            {
                error = "empty auth session response";
                return false;
            }

            try
            {
                return TryMapAuthSessionResponse(
                    JsonUtility.FromJson<MetaAuthSessionResponse>(json),
                    out session,
                    out error);
            }
            catch (ArgumentException exception)
            {
                error = exception.Message;
                return false;
            }
        }

        public static bool TryMapAuthSessionResponse(
            MetaAuthSessionResponse response,
            out MetaAuthSession session,
            out string error)
        {
            session = MetaAuthSession.Unauthenticated;
            error = string.Empty;
            if (response == null)
            {
                error = "missing auth session response";
                return false;
            }

            if (!response.authenticated)
            {
                return true;
            }

            if (response.accountId <= 0L)
            {
                error = "invalid auth session account";
                return false;
            }

            session = new MetaAuthSession(
                true,
                response.accountId,
                response.nicknameRequired,
                response.nickname);
            return true;
        }

        public static bool TryParseNicknameAvailabilityJson(
            string json,
            out MetaNicknameAvailability availability,
            out string error)
        {
            availability = default;
            error = string.Empty;
            if (string.IsNullOrWhiteSpace(json))
            {
                error = "empty nickname availability response";
                return false;
            }

            try
            {
                return TryMapNicknameAvailabilityResponse(
                    JsonUtility.FromJson<MetaNicknameAvailabilityResponse>(json),
                    out availability,
                    out error);
            }
            catch (ArgumentException exception)
            {
                error = exception.Message;
                return false;
            }
        }

        public static bool TryMapNicknameAvailabilityResponse(
            MetaNicknameAvailabilityResponse response,
            out MetaNicknameAvailability availability,
            out string error)
        {
            availability = default;
            error = string.Empty;
            if (response == null)
            {
                error = "missing nickname availability response";
                return false;
            }

            availability = new MetaNicknameAvailability(response.available, response.message);
            return true;
        }

        public static bool TryParseAdmissionJson(
            string json,
            out MetaQueueStatus status,
            out MetaAdmissionResult admission,
            out string error)
        {
            status = default;
            admission = MetaAdmissionResult.Empty;
            error = string.Empty;
            if (string.IsNullOrWhiteSpace(json))
            {
                error = "empty admission response";
                return false;
            }

            try
            {
                return TryMapAdmissionResponse(
                    JsonUtility.FromJson<MetaAdmissionResponse>(json),
                    out status,
                    out admission,
                    out error);
            }
            catch (ArgumentException exception)
            {
                error = exception.Message;
                return false;
            }
        }

        public static bool TryParseQueueStatusJson(
            string json,
            out MetaQueueStatus status,
            out string error)
        {
            return TryParseQueueStatusJson(
                json,
                out status,
                out _,
                out error);
        }

        public static bool TryParseQueueStatusJson(
            string json,
            out MetaQueueStatus status,
            out MetaAdmissionResult admission,
            out string error)
        {
            status = default;
            admission = MetaAdmissionResult.Empty;
            error = string.Empty;
            if (string.IsNullOrWhiteSpace(json))
            {
                error = "empty queue response";
                return false;
            }

            try
            {
                return TryMapQueueStatusResponse(
                    JsonUtility.FromJson<MetaQueueStatusResponse>(json),
                    out status,
                    out admission,
                    out error);
            }
            catch (ArgumentException exception)
            {
                error = exception.Message;
                return false;
            }
        }

        public static bool TryMapAdmissionResponse(
            MetaAdmissionResponse response,
            out MetaQueueStatus status,
            out MetaAdmissionResult admission,
            out string error)
        {
            status = default;
            admission = MetaAdmissionResult.Empty;
            error = string.Empty;
            if (response == null)
            {
                error = "missing admission response";
                return false;
            }

            if (!TryParseQueueState(FirstNonBlank(response.state, response.status), out MetaQueueState state))
            {
                error = "unknown admission state";
                return false;
            }

            EndpointFields endpointFields = ResolveEndpointFields(
                response.gameServerEndpoint,
                response.gameServerHost,
                response.tcpPort,
                response.rudpPort);
            return TryMapQueueFields(
                state,
                response.position,
                response.queueToken,
                response.gameSessionToken,
                response.reservationExpiresAt,
                endpointFields.Host,
                endpointFields.TcpPort,
                endpointFields.RudpPort,
                out status,
                out admission,
                out error);
        }

        public static bool TryMapQueueStatusResponse(
            MetaQueueStatusResponse response,
            out MetaQueueStatus status,
            out string error)
        {
            return TryMapQueueStatusResponse(
                response,
                out status,
                out _,
                out error);
        }

        public static bool TryMapQueueStatusResponse(
            MetaQueueStatusResponse response,
            out MetaQueueStatus status,
            out MetaAdmissionResult admission,
            out string error)
        {
            status = default;
            admission = MetaAdmissionResult.Empty;
            error = string.Empty;
            if (response == null)
            {
                error = "missing queue response";
                return false;
            }

            if (!TryParseQueueState(FirstNonBlank(response.state, response.status), out MetaQueueState state))
            {
                error = "unknown queue state";
                return false;
            }

            EndpointFields endpointFields = ResolveEndpointFields(
                response.gameServerEndpoint,
                response.gameServerHost,
                response.tcpPort,
                response.rudpPort);
            return TryMapQueueFields(
                state,
                response.position,
                response.queueToken,
                response.gameSessionToken,
                response.reservationExpiresAt,
                endpointFields.Host,
                endpointFields.TcpPort,
                endpointFields.RudpPort,
                out status,
                out admission,
                out error);
        }

        public static string NicknameFailureMessage(MetaNicknameValidationFailure failure)
        {
            switch (failure)
            {
                case MetaNicknameValidationFailure.Missing:
                    return "닉네임을 입력해주세요";
                case MetaNicknameValidationFailure.Length:
                    return "닉네임은 2자 이상 12자 이하로 입력해주세요";
                case MetaNicknameValidationFailure.TooLong:
                    return "닉네임이 너무 깁니다";
                case MetaNicknameValidationFailure.UnsupportedCharacters:
                    return "닉네임은 한글, 영문, 숫자만 사용할 수 있습니다";
                case MetaNicknameValidationFailure.Duplicate:
                    return "이미 사용 중인 닉네임입니다";
                case MetaNicknameValidationFailure.AlreadySet:
                    return "이미 닉네임이 설정되어 있습니다";
                default:
                    return "입장에 실패했습니다. 다시 시도해주세요";
            }
        }

        private static bool TryMapQueueFields(
            MetaQueueState state,
            int position,
            string queueToken,
            string gameSessionToken,
            long reservationExpiresAt,
            string gameServerHost,
            int tcpPort,
            int rudpPort,
            out MetaQueueStatus status,
            out MetaAdmissionResult admission,
            out string error)
        {
            status = new MetaQueueStatus(state, position, queueToken);
            admission = MetaAdmissionResult.Empty;
            error = string.Empty;

            if (state != MetaQueueState.Admitted)
            {
                return true;
            }

            if (reservationExpiresAt <= 0L)
            {
                status = default;
                error = "invalid admission reservation deadline";
                return false;
            }

            if (string.IsNullOrWhiteSpace(gameSessionToken) ||
                !PlayerServerEndpoint.TryCreate(
                    gameServerHost,
                    tcpPort,
                    rudpPort,
                    out PlayerServerEndpoint endpoint))
            {
                status = default;
                error = "invalid admission endpoint";
                return false;
            }

            admission = new MetaAdmissionResult(
                gameSessionToken,
                endpoint,
                reservationExpiresAt);
            status = new MetaQueueStatus(MetaQueueState.Admitted, 0, queueToken);
            return true;
        }

        private static bool TryParseQueueState(string rawState, out MetaQueueState state)
        {
            state = MetaQueueState.NotQueued;
            if (string.IsNullOrWhiteSpace(rawState))
            {
                return false;
            }

            string normalized = rawState.Trim()
                .Replace("_", string.Empty)
                .Replace("-", string.Empty)
                .ToUpperInvariant();
            switch (normalized)
            {
                case "QUEUED":
                    state = MetaQueueState.Queued;
                    return true;
                case "ADMITTED":
                    state = MetaQueueState.Admitted;
                    return true;
                case "GAMESERVERREADINESSFAILED":
                    state = MetaQueueState.GameServerReadinessFailed;
                    return true;
                case "QUEUEEXPIRED":
                    state = MetaQueueState.QueueExpired;
                    return true;
                case "NOTQUEUED":
                    state = MetaQueueState.NotQueued;
                    return true;
                default:
                    return false;
            }
        }

        private static string EscapeQueueToken(string queueToken)
        {
            return Uri.EscapeDataString((queueToken ?? string.Empty).Trim());
        }

        private static string FirstNonBlank(string first, string second)
        {
            return !string.IsNullOrWhiteSpace(first) ? first : second;
        }

        private static EndpointFields ResolveEndpointFields(
            MetaGameServerEndpointResponse nestedEndpoint,
            string flatHost,
            int flatTcpPort,
            int flatRudpPort)
        {
            if (nestedEndpoint != null)
            {
                return new EndpointFields(
                    nestedEndpoint.host,
                    nestedEndpoint.tcpPort,
                    nestedEndpoint.rudpPort);
            }

            return new EndpointFields(flatHost, flatTcpPort, flatRudpPort);
        }

        private readonly struct EndpointFields
        {
            public readonly string Host;
            public readonly int TcpPort;
            public readonly int RudpPort;

            public EndpointFields(string host, int tcpPort, int rudpPort)
            {
                Host = host;
                TcpPort = tcpPort;
                RudpPort = rudpPort;
            }
        }
    }
}
