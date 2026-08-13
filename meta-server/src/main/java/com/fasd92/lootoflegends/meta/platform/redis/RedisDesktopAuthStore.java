package com.fasd92.lootoflegends.meta.platform.redis;

import com.fasd92.lootoflegends.meta.identity.application.DesktopAuthStore;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.util.Optional;
import org.springframework.dao.DataAccessException;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.stereotype.Component;

@Component
public final class RedisDesktopAuthStore implements DesktopAuthStore {
  private static final String ATTEMPT_PREFIX = "desktop-auth:v1:attempt:";
  private static final String HANDOFF_PREFIX = "desktop-auth:v1:handoff:";
  private static final String SESSION_PREFIX = "desktop-auth:v1:session:";
  private static final String EVIDENCE_ADMISSION_PREFIX = "evidence-admission:v1:";

  private final StringRedisTemplate redis;
  private final ObjectMapper json;
  private final Clock clock;

  public RedisDesktopAuthStore(StringRedisTemplate redis, ObjectMapper json, Clock clock) {
    this.redis = redis;
    this.json = json;
    this.clock = clock;
  }

  @Override
  public boolean putAttemptIfAbsent(String lookupHash, Attempt attempt) {
    return putIfAbsent(ATTEMPT_PREFIX + lookupHash, attempt, attempt.expiresAt());
  }

  @Override
  public Optional<Attempt> takeAttempt(String lookupHash) {
    return take(ATTEMPT_PREFIX + lookupHash, Attempt.class);
  }

  @Override
  public boolean putHandoffIfAbsent(String lookupHash, Handoff handoff) {
    return putIfAbsent(HANDOFF_PREFIX + lookupHash, handoff, handoff.expiresAt());
  }

  @Override
  public Optional<Handoff> takeHandoff(String lookupHash) {
    return take(HANDOFF_PREFIX + lookupHash, Handoff.class);
  }

  @Override
  public boolean putSessionIfAbsent(String lookupHash, MetaSession session) {
    return putIfAbsent(SESSION_PREFIX + lookupHash, session, session.expiresAt());
  }

  @Override
  public Optional<MetaSession> findSession(String lookupHash) {
    return find(SESSION_PREFIX + lookupHash, MetaSession.class);
  }

  @Override
  public boolean putEvidenceAdmissionIfAbsent(String identityHash, Instant expiresAt) {
    return putIfAbsent(EVIDENCE_ADMISSION_PREFIX + identityHash, "issued", expiresAt);
  }

  private boolean putIfAbsent(String key, Object value, Instant expiresAt) {
    Duration ttl = Duration.between(clock.instant(), expiresAt);
    if (ttl.isNegative() || ttl.isZero()) {
      return false;
    }
    try {
      return Boolean.TRUE.equals(redis.opsForValue().setIfAbsent(key, json(value), ttl));
    } catch (DataAccessException exception) {
      throw new DesktopAuthStore.Unavailable(exception);
    }
  }

  private <T> Optional<T> take(String key, Class<T> type) {
    try {
      String value = redis.opsForValue().getAndDelete(key);
      return value == null ? Optional.empty() : Optional.of(read(value, type));
    } catch (DataAccessException exception) {
      throw new DesktopAuthStore.Unavailable(exception);
    }
  }

  private <T> Optional<T> find(String key, Class<T> type) {
    try {
      String value = redis.opsForValue().get(key);
      return value == null ? Optional.empty() : Optional.of(read(value, type));
    } catch (DataAccessException exception) {
      throw new DesktopAuthStore.Unavailable(exception);
    }
  }

  private String json(Object value) {
    try {
      return json.writeValueAsString(value);
    } catch (JsonProcessingException exception) {
      throw new DesktopAuthStore.Unavailable(exception);
    }
  }

  private <T> T read(String value, Class<T> type) {
    try {
      return json.readValue(value, type);
    } catch (JsonProcessingException exception) {
      throw new DesktopAuthStore.Unavailable(exception);
    }
  }
}
