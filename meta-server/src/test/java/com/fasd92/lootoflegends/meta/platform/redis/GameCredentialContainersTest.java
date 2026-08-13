package com.fasd92.lootoflegends.meta.platform.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialResult;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimRejectionReason;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.io.IOException;
import java.sql.DriverManager;
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
import org.testcontainers.containers.MySQLContainer;
import org.testcontainers.utility.DockerImageName;

class GameCredentialContainersTest {
  private static final Properties IMAGES = loadImages();
  private static final String MYSQL_IMAGE = IMAGES.getProperty("loot.testcontainers.mysql.image");
  private static final String REDIS_IMAGE = IMAGES.getProperty("loot.testcontainers.redis.image");
  private static final MySQLContainer<?> MYSQL =
      new MySQLContainer<>(DockerImageName.parse(MYSQL_IMAGE).asCompatibleSubstituteFor("mysql"))
          .withDatabaseName("loot");
  private static final GenericContainer<?> REDIS =
      new GenericContainer<>(DockerImageName.parse(REDIS_IMAGE)).withExposedPorts(6379);

  private static LettuceConnectionFactory connectionFactory;
  private static StringRedisTemplate redis;
  private static RedisGameCredentialStore store;

  @BeforeAll
  static void startDependencies() {
    MYSQL.start();
    REDIS.start();
    connectionFactory = new LettuceConnectionFactory(REDIS.getHost(), REDIS.getMappedPort(6379));
    connectionFactory.afterPropertiesSet();
    redis = new StringRedisTemplate(connectionFactory);
    redis.afterPropertiesSet();
    store = new RedisGameCredentialStore(redis);
  }

  @AfterEach
  void clearRedis() {
    try (var connection = connectionFactory.getConnection()) {
      connection.serverCommands().flushDb();
    }
  }

  @AfterAll
  static void stopDependencies() {
    if (connectionFactory != null) {
      connectionFactory.destroy();
    }
    REDIS.stop();
    MYSQL.stop();
  }

  @Test
  void pinnedDependenciesAreReachable() throws Exception {
    try (var connection =
            DriverManager.getConnection(
                MYSQL.getJdbcUrl(), MYSQL.getUsername(), MYSQL.getPassword());
        var statement = connection.createStatement();
        var result = statement.executeQuery("SELECT 1")) {
      assertTrue(result.next());
      assertEquals(1, result.getInt(1));
    }
    try (var connection = redis.getConnectionFactory().getConnection()) {
      assertEquals("PONG", connection.ping());
    }
  }

  @Test
  void redisAtomicallyClaimsOnceAndPreservesAudienceAndExpiry() {
    Instant now = Instant.now();
    AccountId accountId = new AccountId(UUID.fromString("00000000-0000-4000-8000-000000000001"));

    assertTrue(
        store.putIfAbsent(
            "hash-one",
            accountId,
            "player-one",
            "loot-game-server-v1",
            now.plusSeconds(30),
            now.plusSeconds(60)));
    assertRejected(store.claim("hash-one", "wrong", now), ClaimRejectionReason.WRONG_AUDIENCE);
    assertInstanceOf(
        ClaimGameCredentialResult.Claimed.class,
        store.claim("hash-one", "loot-game-server-v1", now));
    assertRejected(
        store.claim("hash-one", "loot-game-server-v1", now), ClaimRejectionReason.ALREADY_CONSUMED);

    assertTrue(
        store.putIfAbsent(
            "hash-expired",
            accountId,
            "player-one",
            "loot-game-server-v1",
            now.minusSeconds(1),
            now.plusSeconds(30)));
    assertRejected(
        store.claim("hash-expired", "loot-game-server-v1", now), ClaimRejectionReason.EXPIRED);
  }

  private static void assertRejected(
      ClaimGameCredentialResult result, ClaimRejectionReason expected) {
    var rejected = assertInstanceOf(ClaimGameCredentialResult.Rejected.class, result);
    assertEquals(expected, rejected.reason());
  }

  private static Properties loadImages() {
    var properties = new Properties();
    try (var stream =
        GameCredentialContainersTest.class
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
