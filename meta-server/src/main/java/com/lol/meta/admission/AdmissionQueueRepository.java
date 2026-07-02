package com.lol.meta.admission;

import java.time.Duration;
import java.time.Instant;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.data.redis.core.script.RedisScript;
import org.springframework.stereotype.Repository;

public interface AdmissionQueueRepository {

  boolean hasCapacityAvailable(int capacity);

  int activeSessionCount();

  boolean hasActiveSession(long accountId);

  Optional<String> activeSessionId(long accountId);

  Optional<AdmissionReservation> findPendingReservation(long accountId, Instant now);

  AdmissionReservationCreation createReservationIfCapacityAvailable(
      AdmissionReservation reservation, int capacity, Instant now);

  AdmissionReservationCreation createReplacementReservation(
      AdmissionReservation reservation, Instant now);

  boolean promoteQueuedHeadIfCapacityAvailable(
      String queueToken, AdmissionReservation reservation, int capacity, Instant now);

  AdmissionQueuePosition enqueue(AdmissionQueueEntry entry, Instant now);

  Optional<AdmissionQueuePosition> findQueuePosition(long accountId, Instant now);

  Optional<AdmissionQueuePosition> findPendingQueuePosition(long accountId, Instant now);

  AdmissionQueueLookup findQueuePosition(String queueToken, Instant now);

  void renewQueueEntry(String queueToken, Instant expiresAt);

  boolean cancelQueueEntry(String queueToken);

  boolean removeQueueEntry(String queueToken);

  boolean removePendingReservation(AdmissionReservation reservation);
}

@Repository
class RedisAdmissionQueueRepository implements AdmissionQueueRepository {

  private static final String ACTIVE_SESSIONS_KEY = "release0:active_sessions";
  private static final String QUEUE_KEY = "release0:queue";
  private static final String QUEUE_ENTRY_PREFIX = "release0:queue_entry:";
  private static final String RESERVATION_PREFIX = "release0:reservation:";
  private static final String ACCOUNT_ACTIVE_SESSION_PREFIX = "release0:account_active_session:";
  private static final String ACCOUNT_PENDING_RESERVATION_PREFIX =
      "release0:account_pending_reservation:";
  private static final String ACCOUNT_PENDING_QUEUE_PREFIX = "release0:account_pending_queue:";
  private static final String RESERVATION_MEMBER_PREFIX = "reservation:";

  private static final RedisScript<List> RESERVE_IF_CAPACITY_SCRIPT =
      RedisScript.of(
          """
          local now = tonumber(ARGV[7])
          local pendingToken = redis.call('GET', KEYS[3])
          if pendingToken and pendingToken ~= '' then
            local pendingReservationKey = ARGV[10] .. pendingToken
            local pendingExpiresAt =
                tonumber(redis.call('HGET', pendingReservationKey, 'reservationExpiresAt'))
            local pendingAccountId = redis.call('HGET', pendingReservationKey, 'accountId')
            local pendingNickname = redis.call('HGET', pendingReservationKey, 'nickname')
            if pendingExpiresAt ~= nil
                and pendingExpiresAt > now
                and pendingAccountId == ARGV[3]
                and pendingNickname ~= nil then
              local pendingClientIp = redis.call('HGET', pendingReservationKey, 'clientIp') or ''
              return {
                'EXISTING',
                pendingToken,
                pendingAccountId,
                pendingNickname,
                tostring(pendingExpiresAt),
                '',
                pendingClientIp
              }
            end

            redis.call('SREM', KEYS[1], ARGV[8] .. pendingToken)
            redis.call('DEL', pendingReservationKey)
            redis.call('DEL', KEYS[3])
          end

          local members = redis.call('SMEMBERS', KEYS[1])
          for _, member in ipairs(members) do
            if string.sub(member, 1, string.len(ARGV[8])) == ARGV[8] then
              local token = string.sub(member, string.len(ARGV[8]) + 1)
              local reservationKey = ARGV[10] .. token
              local accountId = redis.call('HGET', reservationKey, 'accountId')
              local expiresAt = tonumber(redis.call('HGET', reservationKey, 'reservationExpiresAt'))
              if expiresAt == nil or now >= expiresAt then
                redis.call('SREM', KEYS[1], member)
                redis.call('DEL', reservationKey)
                if accountId ~= nil then
                  local accountPendingKey = ARGV[11] .. accountId
                  if redis.call('GET', accountPendingKey) == token then
                    redis.call('DEL', accountPendingKey)
                  end
                end
              end
            end
          end

          if redis.call('SCARD', KEYS[1]) >= tonumber(ARGV[1]) then
            return {'CAPACITY_FULL'}
          end

          redis.call('SADD', KEYS[1], ARGV[2])
          redis.call(
            'HSET',
            KEYS[2],
            'accountId',
            ARGV[3],
            'nickname',
            ARGV[4],
            'clientIp',
            ARGV[12],
            'reservationExpiresAt',
            ARGV[5],
            'activeSessionMember',
            ARGV[2])
          redis.call('PEXPIRE', KEYS[2], tonumber(ARGV[6]))
          redis.call('SET', KEYS[3], ARGV[9], 'PX', tonumber(ARGV[6]))
          return {'CREATED', ARGV[9], ARGV[3], ARGV[4], ARGV[5], '', ARGV[12]}
          """,
          List.class);

  private static final RedisScript<List> ENQUEUE_SCRIPT =
      RedisScript.of(
          """
          local now = tonumber(ARGV[5])
          local tokens = redis.call('LRANGE', KEYS[1], 0, -1)
          for _, token in ipairs(tokens) do
            local entryKey = ARGV[6] .. token
            local accountId = redis.call('HGET', entryKey, 'accountId')
            local expiresAt = tonumber(redis.call('HGET', entryKey, 'expiresAt'))
            if accountId == nil or expiresAt == nil or now >= expiresAt then
              redis.call('LREM', KEYS[1], 0, token)
              redis.call('DEL', entryKey)
              if accountId ~= nil then
                local pendingQueueKey = ARGV[7] .. accountId
                if redis.call('GET', pendingQueueKey) == token then
                  redis.call('DEL', pendingQueueKey)
                end
              end
            end
          end

          local existingToken = redis.call('GET', KEYS[3])
          if existingToken and existingToken ~= '' then
            local existingEntryKey = ARGV[6] .. existingToken
            local existingAccountId = redis.call('HGET', existingEntryKey, 'accountId')
            local existingExpiresAt = tonumber(redis.call('HGET', existingEntryKey, 'expiresAt'))
            if existingAccountId == ARGV[1] and existingExpiresAt ~= nil and now < existingExpiresAt then
              local position = 0
              local refreshedTokens = redis.call('LRANGE', KEYS[1], 0, -1)
              for _, token in ipairs(refreshedTokens) do
                local entryKey = ARGV[6] .. token
                local accountId = redis.call('HGET', entryKey, 'accountId')
                local expiresAt = tonumber(redis.call('HGET', entryKey, 'expiresAt'))
                if accountId ~= nil and expiresAt ~= nil and now < expiresAt then
                  position = position + 1
                  if token == existingToken then
                    return {existingToken, tostring(position)}
                  end
                end
              end
            end

            redis.call('LREM', KEYS[1], 0, existingToken)
            redis.call('DEL', existingEntryKey)
            redis.call('DEL', KEYS[3])
          end

          redis.call(
            'HSET',
            KEYS[2],
            'accountId',
            ARGV[1],
            'expiresAt',
            ARGV[2])
          redis.call('PEXPIRE', KEYS[2], tonumber(ARGV[3]))
          redis.call('RPUSH', KEYS[1], ARGV[4])
          redis.call('SET', KEYS[3], ARGV[4], 'PX', tonumber(ARGV[3]))
          return {ARGV[4], tostring(redis.call('LLEN', KEYS[1]))}
          """,
          List.class);

  private static final RedisScript<List> REPLACE_IF_ABSENT_SCRIPT =
      RedisScript.of(
          """
          local now = tonumber(ARGV[4])
          local pendingToken = redis.call('GET', KEYS[3])
          if pendingToken and pendingToken ~= '' then
            local pendingReservationKey = ARGV[8] .. pendingToken
            local pendingExpiresAt =
                tonumber(redis.call('HGET', pendingReservationKey, 'reservationExpiresAt'))
            local pendingAccountId = redis.call('HGET', pendingReservationKey, 'accountId')
            local pendingNickname = redis.call('HGET', pendingReservationKey, 'nickname')
            local pendingReplacedSessionId =
                redis.call('HGET', pendingReservationKey, 'replacedSessionId') or ''
            if pendingExpiresAt ~= nil
                and pendingExpiresAt > now
                and pendingAccountId == ARGV[1]
                and pendingNickname ~= nil then
              local pendingClientIp = redis.call('HGET', pendingReservationKey, 'clientIp') or ''
              return {
                'EXISTING',
                pendingToken,
                pendingAccountId,
                pendingNickname,
                tostring(pendingExpiresAt),
                pendingReplacedSessionId,
                pendingClientIp
              }
            end

            redis.call('SREM', KEYS[1], ARGV[9] .. pendingToken)
            redis.call('DEL', pendingReservationKey)
            redis.call('DEL', KEYS[3])
          end

          redis.call(
            'HSET',
            KEYS[2],
            'accountId',
            ARGV[1],
            'nickname',
            ARGV[2],
            'clientIp',
            ARGV[10],
            'reservationExpiresAt',
            ARGV[3],
            'activeSessionMember',
            '',
            'replacedSessionId',
            ARGV[7])
          redis.call('PEXPIRE', KEYS[2], tonumber(ARGV[5]))
          redis.call('SET', KEYS[3], ARGV[6], 'PX', tonumber(ARGV[5]))
          return {'CREATED', ARGV[6], ARGV[1], ARGV[2], ARGV[3], ARGV[7], ARGV[10]}
          """,
          List.class);

  private static final RedisScript<Long> PROMOTE_HEAD_IF_CAPACITY_SCRIPT =
      RedisScript.of(
          """
          local members = redis.call('SMEMBERS', KEYS[1])
          for _, member in ipairs(members) do
            if string.sub(member, 1, string.len(ARGV[8])) == ARGV[8] then
              local token = string.sub(member, string.len(ARGV[8]) + 1)
              local reservationKey = ARGV[9] .. token
              local accountId = redis.call('HGET', reservationKey, 'accountId')
              local expiresAt = tonumber(redis.call('HGET', reservationKey, 'reservationExpiresAt'))
              if expiresAt == nil or tonumber(ARGV[7]) >= expiresAt then
                redis.call('SREM', KEYS[1], member)
                redis.call('DEL', reservationKey)
                if accountId ~= nil then
                  local accountPendingKey = ARGV[12] .. accountId
                  if redis.call('GET', accountPendingKey) == token then
                    redis.call('DEL', accountPendingKey)
                  end
                end
              end
            end
          end

          if redis.call('LINDEX', KEYS[2], 0) ~= ARGV[2] then
            return 0
          end

          local entryAccountId = redis.call('HGET', KEYS[3], 'accountId')
          local entryExpiresAt = tonumber(redis.call('HGET', KEYS[3], 'expiresAt'))
          if entryAccountId == nil or entryExpiresAt == nil then
            redis.call('LREM', KEYS[2], 0, ARGV[2])
            redis.call('DEL', KEYS[3])
            if redis.call('GET', KEYS[5]) == ARGV[2] then
              redis.call('DEL', KEYS[5])
            end
            return 0
          end

          if entryAccountId ~= ARGV[4] or tonumber(ARGV[7]) >= entryExpiresAt then
            redis.call('LREM', KEYS[2], 0, ARGV[2])
            redis.call('DEL', KEYS[3])
            if redis.call('GET', KEYS[5]) == ARGV[2] then
              redis.call('DEL', KEYS[5])
            end
            return 0
          end

          if redis.call('SCARD', KEYS[1]) >= tonumber(ARGV[1]) then
            return 0
          end

          redis.call('SADD', KEYS[1], ARGV[3])
          redis.call(
            'HSET',
            KEYS[4],
            'accountId',
            ARGV[4],
            'nickname',
            ARGV[5],
            'clientIp',
            ARGV[13],
            'reservationExpiresAt',
            ARGV[6],
            'activeSessionMember',
            ARGV[3])
          redis.call('PEXPIRE', KEYS[4], tonumber(ARGV[10]))
          redis.call('SET', KEYS[6], ARGV[11], 'PX', tonumber(ARGV[10]))
          redis.call('LREM', KEYS[2], 0, ARGV[2])
          redis.call('DEL', KEYS[3])
          if redis.call('GET', KEYS[5]) == ARGV[2] then
            redis.call('DEL', KEYS[5])
          end
          return 1
          """,
          Long.class);

  private final StringRedisTemplate redisTemplate;

  RedisAdmissionQueueRepository(StringRedisTemplate redisTemplate) {
    this.redisTemplate = redisTemplate;
  }

  @Override
  public boolean hasCapacityAvailable(int capacity) {
    return activeSessionCount() < capacity;
  }

  @Override
  public int activeSessionCount() {
    Long count = redisTemplate.opsForSet().size(ACTIVE_SESSIONS_KEY);
    return count == null ? 0 : Math.toIntExact(count);
  }

  @Override
  public boolean hasActiveSession(long accountId) {
    return activeSessionId(accountId).isPresent();
  }

  @Override
  public Optional<String> activeSessionId(long accountId) {
    return Optional.ofNullable(
        redisTemplate.opsForValue().get(accountActiveSessionKey(accountId)));
  }

  @Override
  public Optional<AdmissionReservation> findPendingReservation(long accountId, Instant now) {
    String token = redisTemplate.opsForValue().get(accountPendingReservationKey(accountId));
    if (token == null || token.isBlank()) {
      return Optional.empty();
    }

    Optional<AdmissionReservation> reservation = readReservation(token, now);
    if (reservation.isPresent() && reservation.get().accountId() == accountId) {
      return reservation;
    }

    removeReservation(token, accountId);
    return Optional.empty();
  }

  @Override
  @SuppressWarnings("unchecked")
  public AdmissionReservationCreation createReservationIfCapacityAvailable(
      AdmissionReservation reservation, int capacity, Instant now) {
    List<String> result =
        (List<String>)
            redisTemplate.execute(
                RESERVE_IF_CAPACITY_SCRIPT,
                List.of(
                    ACTIVE_SESSIONS_KEY,
                    reservationKey(reservation.gameSessionToken()),
                    accountPendingReservationKey(reservation.accountId())),
                Integer.toString(capacity),
                reservationMember(reservation.gameSessionToken()),
                Long.toString(reservation.accountId()),
                reservation.nickname(),
                Long.toString(reservation.reservationExpiresAt().toEpochMilli()),
                Long.toString(ttlMillis(reservation.ttl())),
                Long.toString(now.toEpochMilli()),
                RESERVATION_MEMBER_PREFIX,
                reservation.gameSessionToken(),
                RESERVATION_PREFIX,
                ACCOUNT_PENDING_RESERVATION_PREFIX,
                reservation.clientIp());
    return reservationCreationFrom(result, reservation.ttl(), now);
  }

  @Override
  @SuppressWarnings("unchecked")
  public AdmissionReservationCreation createReplacementReservation(
      AdmissionReservation reservation, Instant now) {
    List<String> result =
        (List<String>)
            redisTemplate.execute(
                REPLACE_IF_ABSENT_SCRIPT,
                List.of(
                    ACTIVE_SESSIONS_KEY,
                    reservationKey(reservation.gameSessionToken()),
                    accountPendingReservationKey(reservation.accountId())),
                Long.toString(reservation.accountId()),
                reservation.nickname(),
                Long.toString(reservation.reservationExpiresAt().toEpochMilli()),
                Long.toString(now.toEpochMilli()),
                Long.toString(ttlMillis(reservation.ttl())),
                reservation.gameSessionToken(),
                reservation.replacedSessionId() == null ? "" : reservation.replacedSessionId(),
                RESERVATION_PREFIX,
                RESERVATION_MEMBER_PREFIX,
                reservation.clientIp());
    return reservationCreationFrom(result, reservation.ttl(), now);
  }

  @Override
  public boolean promoteQueuedHeadIfCapacityAvailable(
      String queueToken, AdmissionReservation reservation, int capacity, Instant now) {
    Long result =
        redisTemplate.execute(
            PROMOTE_HEAD_IF_CAPACITY_SCRIPT,
            List.of(
                ACTIVE_SESSIONS_KEY,
                QUEUE_KEY,
                queueEntryKey(queueToken),
                reservationKey(reservation.gameSessionToken()),
                accountPendingQueueKey(reservation.accountId()),
                accountPendingReservationKey(reservation.accountId())),
            Integer.toString(capacity),
            queueToken,
            reservationMember(reservation.gameSessionToken()),
            Long.toString(reservation.accountId()),
            reservation.nickname(),
            Long.toString(reservation.reservationExpiresAt().toEpochMilli()),
            Long.toString(now.toEpochMilli()),
            RESERVATION_MEMBER_PREFIX,
            RESERVATION_PREFIX,
            Long.toString(ttlMillis(reservation.ttl())),
            reservation.gameSessionToken(),
            ACCOUNT_PENDING_RESERVATION_PREFIX,
            reservation.clientIp());
    return Objects.equals(result, 1L);
  }

  @Override
  @SuppressWarnings("unchecked")
  public AdmissionQueuePosition enqueue(AdmissionQueueEntry entry, Instant now) {
    long ttlMillis =
        Math.max(1L, entry.expiresAt().toEpochMilli() - now.toEpochMilli());
    List<String> result =
        (List<String>)
            redisTemplate.execute(
                ENQUEUE_SCRIPT,
                List.of(
                    QUEUE_KEY,
                    queueEntryKey(entry.queueToken()),
                    accountPendingQueueKey(entry.accountId())),
                Long.toString(entry.accountId()),
                Long.toString(entry.expiresAt().toEpochMilli()),
                Long.toString(ttlMillis),
                entry.queueToken(),
                Long.toString(now.toEpochMilli()),
                QUEUE_ENTRY_PREFIX,
                ACCOUNT_PENDING_QUEUE_PREFIX);
    if (result == null || result.size() < 2) {
      return new AdmissionQueuePosition(entry.queueToken(), entry.accountId(), 1);
    }
    return new AdmissionQueuePosition(
        result.get(0), entry.accountId(), Integer.parseInt(result.get(1)));
  }

  @Override
  public Optional<AdmissionQueuePosition> findQueuePosition(long accountId, Instant now) {
    List<String> queueTokens = redisTemplate.opsForList().range(QUEUE_KEY, 0, -1);
    if (queueTokens == null || queueTokens.isEmpty()) {
      return Optional.empty();
    }

    int visiblePosition = 0;
    for (String queueToken : queueTokens) {
      String entryKey = queueEntryKey(queueToken);
      Object storedAccountId = redisTemplate.opsForHash().get(entryKey, "accountId");
      Object expiresAt = redisTemplate.opsForHash().get(entryKey, "expiresAt");
      if (storedAccountId == null || expiresAt == null) {
        removeQueueEntry(queueToken);
        continue;
      }

      if (Long.parseLong(expiresAt.toString()) <= now.toEpochMilli()) {
        removeQueueEntry(queueToken);
        continue;
      }

      visiblePosition += 1;
      if (Long.toString(accountId).equals(storedAccountId.toString())) {
        return Optional.of(new AdmissionQueuePosition(queueToken, accountId, visiblePosition));
      }
    }
    return Optional.empty();
  }

  @Override
  public Optional<AdmissionQueuePosition> findPendingQueuePosition(long accountId, Instant now) {
    String queueToken = redisTemplate.opsForValue().get(accountPendingQueueKey(accountId));
    if (queueToken == null || queueToken.isBlank()) {
      return Optional.empty();
    }

    AdmissionQueueLookup lookup = findQueuePosition(queueToken, now);
    if (lookup.status() == AdmissionQueueLookupStatus.QUEUED
        && lookup.position().accountId() == accountId) {
      return Optional.of(lookup.position());
    }

    redisTemplate.delete(accountPendingQueueKey(accountId));
    return Optional.empty();
  }

  @Override
  public AdmissionQueueLookup findQueuePosition(String queueToken, Instant now) {
    List<String> queueTokens = redisTemplate.opsForList().range(QUEUE_KEY, 0, -1);
    if (queueTokens == null || queueTokens.isEmpty() || !queueTokens.contains(queueToken)) {
      return AdmissionQueueLookup.notQueued();
    }

    int visiblePosition = 0;
    for (String currentToken : queueTokens) {
      String entryKey = queueEntryKey(currentToken);
      Object storedAccountId = redisTemplate.opsForHash().get(entryKey, "accountId");
      Object expiresAt = redisTemplate.opsForHash().get(entryKey, "expiresAt");
      if (storedAccountId == null || expiresAt == null) {
        removeQueueEntry(currentToken);
        if (currentToken.equals(queueToken)) {
          return AdmissionQueueLookup.expired();
        }
        continue;
      }

      if (Long.parseLong(expiresAt.toString()) <= now.toEpochMilli()) {
        removeQueueEntry(currentToken);
        if (currentToken.equals(queueToken)) {
          return AdmissionQueueLookup.expired();
        }
        continue;
      }

      visiblePosition += 1;
      if (currentToken.equals(queueToken)) {
        return AdmissionQueueLookup.queued(
            new AdmissionQueuePosition(
                queueToken, Long.parseLong(storedAccountId.toString()), visiblePosition));
      }
    }
    return AdmissionQueueLookup.notQueued();
  }

  @Override
  public void renewQueueEntry(String queueToken, Instant expiresAt) {
    String entryKey = queueEntryKey(queueToken);
    Duration ttl =
        Duration.ofMillis(
            Math.max(1L, expiresAt.toEpochMilli() - Instant.now().toEpochMilli()));
    Object accountId = redisTemplate.opsForHash().get(entryKey, "accountId");
    redisTemplate
        .opsForHash()
        .put(entryKey, "expiresAt", Long.toString(expiresAt.toEpochMilli()));
    redisTemplate.expire(entryKey, ttl);
    if (accountId != null) {
      redisTemplate.expire(accountPendingQueueKey(Long.parseLong(accountId.toString())), ttl);
    }
  }

  @Override
  public boolean cancelQueueEntry(String queueToken) {
    return removeQueueEntry(queueToken);
  }

  @Override
  public boolean removeQueueEntry(String queueToken) {
    Object accountId = redisTemplate.opsForHash().get(queueEntryKey(queueToken), "accountId");
    redisTemplate.opsForList().remove(QUEUE_KEY, 0, queueToken);
    Boolean deleted = redisTemplate.delete(queueEntryKey(queueToken));
    if (accountId != null) {
      String pendingQueueKey = accountPendingQueueKey(Long.parseLong(accountId.toString()));
      if (queueToken.equals(redisTemplate.opsForValue().get(pendingQueueKey))) {
        redisTemplate.delete(pendingQueueKey);
      }
    }
    return Boolean.TRUE.equals(deleted);
  }

  @Override
  public boolean removePendingReservation(AdmissionReservation reservation) {
    removeReservation(reservation.gameSessionToken(), reservation.accountId());
    return true;
  }

  private Map<String, String> reservationFields(
      AdmissionReservation reservation, String activeSessionMember) {
    Map<String, String> fields = new LinkedHashMap<>();
    fields.put("accountId", Long.toString(reservation.accountId()));
    fields.put("nickname", reservation.nickname());
    fields.put("clientIp", reservation.clientIp());
    fields.put(
        "reservationExpiresAt",
        Long.toString(reservation.reservationExpiresAt().toEpochMilli()));
    fields.put("activeSessionMember", activeSessionMember);
    return fields;
  }

  private Optional<AdmissionReservation> readReservation(String token, Instant now) {
    String key = reservationKey(token);
    Object accountId = redisTemplate.opsForHash().get(key, "accountId");
    Object nickname = redisTemplate.opsForHash().get(key, "nickname");
    Object clientIp = redisTemplate.opsForHash().get(key, "clientIp");
    Object expiresAt = redisTemplate.opsForHash().get(key, "reservationExpiresAt");
    if (accountId == null || nickname == null || expiresAt == null) {
      return Optional.empty();
    }

    long expiresAtMillis = Long.parseLong(expiresAt.toString());
    if (expiresAtMillis <= now.toEpochMilli()) {
      return Optional.empty();
    }

    return Optional.of(
        new AdmissionReservation(
            token,
            Long.parseLong(accountId.toString()),
            nickname.toString(),
            clientIp == null ? "" : clientIp.toString(),
            Instant.ofEpochMilli(expiresAtMillis),
            Duration.ofMillis(Math.max(1L, expiresAtMillis - now.toEpochMilli())),
            null));
  }

  private void removeReservation(String token, long accountId) {
    redisTemplate.opsForSet().remove(ACTIVE_SESSIONS_KEY, reservationMember(token));
    redisTemplate.delete(reservationKey(token));
    String pendingKey = accountPendingReservationKey(accountId);
    if (token.equals(redisTemplate.opsForValue().get(pendingKey))) {
      redisTemplate.delete(pendingKey);
    }
  }

  private AdmissionReservationCreation reservationCreationFrom(
      List<String> result, Duration ttl, Instant now) {
    if (result == null || result.isEmpty()) {
      return AdmissionReservationCreation.capacityFull();
    }
    if ("CAPACITY_FULL".equals(result.getFirst())) {
      return AdmissionReservationCreation.capacityFull();
    }
    if (result.size() < 5) {
      return AdmissionReservationCreation.capacityFull();
    }

    long expiresAtMillis = Long.parseLong(result.get(4));
    String replacedSessionId = result.size() >= 6 && !result.get(5).isBlank() ? result.get(5) : null;
    String clientIp = result.size() >= 7 ? result.get(6) : "";
    AdmissionReservation reservation =
        new AdmissionReservation(
            result.get(1),
            Long.parseLong(result.get(2)),
            result.get(3),
            clientIp,
            Instant.ofEpochMilli(expiresAtMillis),
            Duration.ofMillis(Math.max(1L, expiresAtMillis - now.toEpochMilli())),
            replacedSessionId);
    if ("EXISTING".equals(result.getFirst())) {
      return AdmissionReservationCreation.existing(reservation);
    }
    return AdmissionReservationCreation.created(
        new AdmissionReservation(
            reservation.gameSessionToken(),
            reservation.accountId(),
            reservation.nickname(),
            reservation.clientIp(),
            reservation.reservationExpiresAt(),
            ttl,
            replacedSessionId));
  }

  private long ttlMillis(Duration ttl) {
    return Math.max(1L, ttl.toMillis());
  }

  private String reservationMember(String gameSessionToken) {
    return RESERVATION_MEMBER_PREFIX + gameSessionToken;
  }

  private String reservationKey(String gameSessionToken) {
    return RESERVATION_PREFIX + gameSessionToken;
  }

  private String queueEntryKey(String queueToken) {
    return QUEUE_ENTRY_PREFIX + queueToken;
  }

  private String accountActiveSessionKey(long accountId) {
    return ACCOUNT_ACTIVE_SESSION_PREFIX + accountId;
  }

  private String accountPendingReservationKey(long accountId) {
    return ACCOUNT_PENDING_RESERVATION_PREFIX + accountId;
  }

  private String accountPendingQueueKey(long accountId) {
    return ACCOUNT_PENDING_QUEUE_PREFIX + accountId;
  }
}
