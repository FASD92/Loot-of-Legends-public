package com.fasd92.lootoflegends.meta.platform.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import com.fasd92.lootoflegends.meta.identity.api.AuthenticatedPrincipal;
import com.fasd92.lootoflegends.meta.identity.application.DesktopAuthStore;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.net.URI;
import java.time.Clock;
import java.time.Instant;
import java.util.Properties;
import java.util.UUID;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;
import org.springframework.data.redis.connection.lettuce.LettuceConnectionFactory;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.testcontainers.containers.GenericContainer;
import org.testcontainers.utility.DockerImageName;

class DesktopAuthStoreContainersTest {
  private static final GenericContainer<?> REDIS =
      new GenericContainer<>(
              DockerImageName.parse(loadImages().getProperty("loot.testcontainers.redis.image")))
          .withExposedPorts(6379);

  private static LettuceConnectionFactory connectionFactory;
  private static StringRedisTemplate redis;
  private static RedisDesktopAuthStore store;

  @BeforeAll
  static void startRedis() {
    REDIS.start();
    connectionFactory = new LettuceConnectionFactory(REDIS.getHost(), REDIS.getMappedPort(6379));
    connectionFactory.afterPropertiesSet();
    redis = new StringRedisTemplate(connectionFactory);
    redis.afterPropertiesSet();
    store =
        new RedisDesktopAuthStore(
            redis, new ObjectMapper().findAndRegisterModules(), Clock.systemUTC());
  }

  @AfterEach
  void clearRedis() {
    try (var connection = connectionFactory.getConnection()) {
      connection.serverCommands().flushDb();
    }
  }

  @AfterAll
  static void stopRedis() {
    if (connectionFactory != null) {
      connectionFactory.destroy();
    }
    REDIS.stop();
  }

  @Test
  void attemptAndHandoffAreAtomicallyTakenOnce() {
    Instant expiresAt = Instant.now().plusSeconds(120);
    var attempt =
        new DesktopAuthStore.Attempt(
            URI.create("http://127.0.0.1:49152/callback"),
            "client-state",
            "client-challenge",
            "provider-verifier",
            expiresAt);
    assertTrue(store.putAttemptIfAbsent("attempt-hash", attempt));
    assertFalse(store.putAttemptIfAbsent("attempt-hash", attempt));
    assertEquals(attempt, store.takeAttempt("attempt-hash").orElseThrow());
    assertTrue(store.takeAttempt("attempt-hash").isEmpty());

    var principal =
        new AuthenticatedPrincipal(
            new AccountId(UUID.fromString("00000000-0000-4000-8000-000000000001")), "player-one");
    var handoff =
        new DesktopAuthStore.Handoff(principal, "client-state", "client-challenge", expiresAt);
    assertTrue(store.putHandoffIfAbsent("handoff-hash", handoff));
    assertEquals(handoff, store.takeHandoff("handoff-hash").orElseThrow());
    assertTrue(store.takeHandoff("handoff-hash").isEmpty());
  }

  @Test
  void metaSessionLookupPreservesPrincipalWithoutRawTokenKey() {
    var principal =
        new AuthenticatedPrincipal(
            new AccountId(UUID.fromString("00000000-0000-4000-8000-000000000001")), "player-one");
    var session = new DesktopAuthStore.MetaSession(principal, Instant.now().plusSeconds(60));

    assertTrue(store.putSessionIfAbsent("session-hash", session));
    assertEquals(session, store.findSession("session-hash").orElseThrow());
    assertTrue(redis.keys("*raw-meta-session*").isEmpty());
  }

  private static Properties loadImages() {
    var properties = new Properties();
    try (var stream =
        DesktopAuthStoreContainersTest.class
            .getClassLoader()
            .getResourceAsStream("testcontainers.properties")) {
      if (stream == null) {
        throw new IllegalStateException("testcontainers.properties is missing");
      }
      properties.load(stream);
      return properties;
    } catch (IOException exception) {
      throw new IllegalStateException("cannot read test container images", exception);
    }
  }
}
