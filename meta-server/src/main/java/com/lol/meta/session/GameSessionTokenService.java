package com.lol.meta.session;

import com.lol.meta.firewall.GameFirewallClient;
import com.lol.meta.firewall.NoopGameFirewallClient;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.data.redis.core.script.RedisScript;
import org.springframework.stereotype.Repository;
import org.springframework.stereotype.Service;

@Service
public final class GameSessionTokenService {

  private static final Duration DEFAULT_ACTIVE_SESSION_FIREWALL_TTL = Duration.ofSeconds(600);

  private final GameSessionTokenRepository repository;
  private final GameFirewallClient firewallClient;
  private final Duration activeSessionFirewallTtl;

  @Autowired
  public GameSessionTokenService(
      GameSessionTokenRepository repository,
      @Value("${release0.firewall.active-session-ttl-seconds}")
          long activeSessionFirewallTtlSeconds,
      GameFirewallClient firewallClient) {
    this(
        repository,
        firewallClient,
        Duration.ofSeconds(activeSessionFirewallTtlSeconds));
  }

  private GameSessionTokenService(
      GameSessionTokenRepository repository,
      GameFirewallClient firewallClient,
      Duration activeSessionFirewallTtl) {
    this.repository = Objects.requireNonNull(repository, "repository");
    this.firewallClient = Objects.requireNonNull(firewallClient, "firewallClient");
    this.activeSessionFirewallTtl =
        Objects.requireNonNull(activeSessionFirewallTtl, "activeSessionFirewallTtl");
  }

  public static GameSessionTokenService forTest(GameSessionTokenRepository repository) {
    return new GameSessionTokenService(
        repository, new NoopGameFirewallClient(), DEFAULT_ACTIVE_SESSION_FIREWALL_TTL);
  }

  static GameSessionTokenService forTest(
      GameSessionTokenRepository repository,
      GameFirewallClient firewallClient,
      Duration activeSessionFirewallTtl) {
    return new GameSessionTokenService(repository, firewallClient, activeSessionFirewallTtl);
  }

  public GameSessionClaimResponse claim(GameSessionClaimRequest request, Instant now) {
    if (request == null
        || isBlank(request.gameSessionToken())
        || isBlank(request.connectionId())
        || now == null) {
      return GameSessionClaimResponse.rejected();
    }

    String gameSessionToken = request.gameSessionToken().trim();
    String connectionId = request.connectionId().trim();
    Optional<ClaimedGameSession> claimedSession =
        repository.claimReservation(gameSessionToken, connectionId, now);
    if (claimedSession.isEmpty()) {
      return GameSessionClaimResponse.rejected();
    }

    ClaimedGameSession session = claimedSession.get();
    renewActiveFirewallSession(session.clientIp(), connectionId);
    return GameSessionClaimResponse.accepted(session);
  }

  public boolean release(GameSessionReleaseRequest request) {
    if (request == null
        || request.accountId() == null
        || request.accountId() <= 0
        || isBlank(request.connectionId())) {
      return false;
    }

    String connectionId = request.connectionId().trim();
    boolean released = repository.releaseActiveSession(request.accountId(), connectionId);
    if (released) {
      closeFirewallSession(connectionId);
    }
    return released;
  }

  public boolean renew(GameSessionRenewRequest request) {
    if (request == null
        || request.accountId() == null
        || request.accountId() <= 0
        || isBlank(request.connectionId())) {
      return false;
    }

    String connectionId = request.connectionId().trim();
    Optional<ActiveGameSession> activeSession =
        repository.findActiveSession(request.accountId(), connectionId);
    if (activeSession.isEmpty() || isBlank(activeSession.get().clientIp())) {
      return false;
    }

    GameFirewallClient.Decision decision =
        firewallClient.renewActiveSession(
            activeSession.get().clientIp().trim(), activeSessionFirewallTtl, connectionId);
    return decision.success();
  }

  private void renewActiveFirewallSession(String clientIp, String connectionId) {
    if (isBlank(clientIp)) {
      return;
    }
    firewallClient.renewActiveSession(
        clientIp.trim(), activeSessionFirewallTtl, connectionId);
  }

  private void closeFirewallSession(String connectionId) {
    firewallClient.closeSession(connectionId);
  }

  private boolean isBlank(String value) {
    return value == null || value.isBlank();
  }
}

interface GameSessionTokenRepository {

  Optional<ClaimedGameSession> claimReservation(
      String gameSessionToken, String connectionId, Instant now);

  boolean releaseActiveSession(long accountId, String connectionId);

  Optional<ActiveGameSession> findActiveSession(long accountId, String connectionId);
}

@Repository
class RedisGameSessionTokenRepository implements GameSessionTokenRepository {

  private static final String ACTIVE_SESSIONS_KEY = "release0:active_sessions";
  private static final String RESERVATION_PREFIX = "release0:reservation:";
  private static final String ACCOUNT_ACTIVE_SESSION_PREFIX = "release0:account_active_session:";
  private static final String ACTIVE_SESSION_METADATA_PREFIX = "release0:active_session:";
  private static final String ACCOUNT_PENDING_RESERVATION_PREFIX =
      "release0:account_pending_reservation:";

  private static final RedisScript<List> CLAIM_SCRIPT =
      RedisScript.of(
          """
          if redis.call('EXISTS', KEYS[1]) == 0 then
            redis.call('SREM', KEYS[2], ARGV[4])
            return nil
          end

          local expiresAt = tonumber(redis.call('HGET', KEYS[1], 'reservationExpiresAt'))
          local activeSessionMember = redis.call('HGET', KEYS[1], 'activeSessionMember')
          local accountId = redis.call('HGET', KEYS[1], 'accountId')
          if expiresAt == nil or tonumber(ARGV[2]) >= expiresAt then
            if activeSessionMember and activeSessionMember ~= '' then
              redis.call('SREM', KEYS[2], activeSessionMember)
            else
              redis.call('SREM', KEYS[2], ARGV[4])
            end
            if accountId ~= nil then
              local pendingReservationKey = ARGV[5] .. accountId
              if redis.call('GET', pendingReservationKey) == ARGV[6] then
                redis.call('DEL', pendingReservationKey)
              end
            end
            redis.call('DEL', KEYS[1])
            return nil
          end

          local nickname = redis.call('HGET', KEYS[1], 'nickname')
          if accountId == nil or nickname == nil then
            if activeSessionMember and activeSessionMember ~= '' then
              redis.call('SREM', KEYS[2], activeSessionMember)
            else
              redis.call('SREM', KEYS[2], ARGV[4])
            end
            if accountId ~= nil then
              local pendingReservationKey = ARGV[5] .. accountId
              if redis.call('GET', pendingReservationKey) == ARGV[6] then
                redis.call('DEL', pendingReservationKey)
              end
            end
            redis.call('DEL', KEYS[1])
            return nil
          end

          local pendingReservationKey = ARGV[5] .. accountId
          local reservationReplacedSessionId = redis.call('HGET', KEYS[1], 'replacedSessionId') or ''
          local clientIp = redis.call('HGET', KEYS[1], 'clientIp') or ''
          local accountActiveSessionKey = ARGV[3] .. accountId
          local currentActiveSessionId = redis.call('GET', accountActiveSessionKey)
          if reservationReplacedSessionId ~= ''
              and currentActiveSessionId ~= reservationReplacedSessionId then
            if activeSessionMember and activeSessionMember ~= '' then
              redis.call('SREM', KEYS[2], activeSessionMember)
            else
              redis.call('SREM', KEYS[2], ARGV[4])
            end
            if redis.call('GET', pendingReservationKey) == ARGV[6] then
              redis.call('DEL', pendingReservationKey)
            end
            redis.call('DEL', KEYS[1])
            return nil
          end

          if activeSessionMember and activeSessionMember ~= '' then
            redis.call('SREM', KEYS[2], activeSessionMember)
          end
          if reservationReplacedSessionId ~= '' then
            redis.call('SREM', KEYS[2], reservationReplacedSessionId)
            redis.call('DEL', ARGV[7] .. reservationReplacedSessionId)
          elseif currentActiveSessionId and currentActiveSessionId ~= '' then
            redis.call('SREM', KEYS[2], currentActiveSessionId)
            redis.call('DEL', ARGV[7] .. currentActiveSessionId)
          end

          redis.call('SADD', KEYS[2], ARGV[1])
          redis.call('SET', accountActiveSessionKey, ARGV[1])
          redis.call(
              'HSET',
              ARGV[7] .. ARGV[1],
              'accountId',
              accountId,
              'clientIp',
              clientIp)
          if redis.call('GET', pendingReservationKey) == ARGV[6] then
            redis.call('DEL', pendingReservationKey)
          end
          redis.call('DEL', KEYS[1])

          return {accountId, nickname, tostring(expiresAt), reservationReplacedSessionId, clientIp}
          """,
          List.class);

  private static final RedisScript<Long> RELEASE_SCRIPT =
      RedisScript.of(
          """
          local currentSessionId = redis.call('GET', KEYS[2])
          if currentSessionId == ARGV[1] then
            local pendingToken = redis.call('GET', KEYS[3])
            if pendingToken and pendingToken ~= '' then
              local reservationKey = ARGV[2] .. pendingToken
              local reservationReplacedSessionId =
                  redis.call('HGET', reservationKey, 'replacedSessionId')
              if reservationReplacedSessionId == ARGV[1] then
                local activeSessionMember =
                    redis.call('HGET', reservationKey, 'activeSessionMember')
                if activeSessionMember and activeSessionMember ~= '' then
                  redis.call('SREM', KEYS[1], activeSessionMember)
                end
                redis.call('DEL', reservationKey)
                redis.call('DEL', KEYS[3])
              end
            end
            redis.call('DEL', KEYS[2])
            redis.call('DEL', KEYS[4])
            redis.call('SREM', KEYS[1], ARGV[1])
            return 1
          end
          return 0
          """,
          Long.class);

  private static final RedisScript<String> FIND_ACTIVE_SESSION_SCRIPT =
      RedisScript.of(
          """
          local currentSessionId = redis.call('GET', KEYS[1])
          if currentSessionId ~= ARGV[1] then
            return nil
          end

          local accountId = redis.call('HGET', KEYS[2], 'accountId')
          local clientIp = redis.call('HGET', KEYS[2], 'clientIp')
          if accountId ~= ARGV[2] or clientIp == nil or clientIp == '' then
            return nil
          end
          return clientIp
          """,
          String.class);

  private final StringRedisTemplate redisTemplate;

  RedisGameSessionTokenRepository(StringRedisTemplate redisTemplate) {
    this.redisTemplate = redisTemplate;
  }

  @Override
  @SuppressWarnings("unchecked")
  public Optional<ClaimedGameSession> claimReservation(
      String gameSessionToken, String connectionId, Instant now) {
    List<String> result =
        (List<String>)
            redisTemplate.execute(
                CLAIM_SCRIPT,
                List.of(reservationKey(gameSessionToken), ACTIVE_SESSIONS_KEY),
                connectionId,
                Long.toString(now.toEpochMilli()),
                ACCOUNT_ACTIVE_SESSION_PREFIX,
                reservationMember(gameSessionToken),
                ACCOUNT_PENDING_RESERVATION_PREFIX,
                gameSessionToken,
                ACTIVE_SESSION_METADATA_PREFIX);

    if (result == null || result.size() < 3) {
      return Optional.empty();
    }

    return Optional.of(toClaimedGameSession(result));
  }

  @Override
  public boolean releaseActiveSession(long accountId, String connectionId) {
    Long result =
        redisTemplate.execute(
            RELEASE_SCRIPT,
            List.of(
                ACTIVE_SESSIONS_KEY,
                accountActiveSessionKey(accountId),
                accountPendingReservationKey(accountId),
                activeSessionMetadataKey(connectionId)),
            connectionId,
            RESERVATION_PREFIX);
    return Objects.equals(result, 1L);
  }

  @Override
  public Optional<ActiveGameSession> findActiveSession(long accountId, String connectionId) {
    String clientIp =
        redisTemplate.execute(
            FIND_ACTIVE_SESSION_SCRIPT,
            List.of(accountActiveSessionKey(accountId), activeSessionMetadataKey(connectionId)),
            connectionId,
            Long.toString(accountId));
    if (clientIp == null || clientIp.isBlank()) {
      return Optional.empty();
    }
    return Optional.of(new ActiveGameSession(accountId, connectionId, clientIp));
  }

  private ClaimedGameSession toClaimedGameSession(List<String> result) {
    String rawReplacedSessionId = result.size() >= 4 ? result.get(3) : null;
    String replacedSessionId =
        rawReplacedSessionId != null && !rawReplacedSessionId.isBlank()
            ? rawReplacedSessionId
            : null;
    String clientIp = result.size() >= 5 && result.get(4) != null ? result.get(4) : "";
    return new ClaimedGameSession(
        Long.parseLong(result.get(0)),
        result.get(1),
        replacedSessionId,
        Instant.ofEpochMilli(Long.parseLong(result.get(2))),
        clientIp);
  }

  private String reservationKey(String gameSessionToken) {
    return RESERVATION_PREFIX + gameSessionToken;
  }

  private String reservationMember(String gameSessionToken) {
    return "reservation:" + gameSessionToken;
  }

  private String accountActiveSessionKey(long accountId) {
    return ACCOUNT_ACTIVE_SESSION_PREFIX + accountId;
  }

  private String activeSessionMetadataKey(String connectionId) {
    return ACTIVE_SESSION_METADATA_PREFIX + connectionId;
  }

  private String accountPendingReservationKey(long accountId) {
    return ACCOUNT_PENDING_RESERVATION_PREFIX + accountId;
  }
}

record GameSessionReservation(
    long accountId,
    String nickname,
    Instant reservationExpiresAt,
    String replacedSessionId,
    String clientIp) {}

record ClaimedGameSession(
    long accountId,
    String nickname,
    String replacedSessionId,
    Instant reservationExpiresAt,
    String clientIp) {}

record ActiveGameSession(long accountId, String connectionId, String clientIp) {}
