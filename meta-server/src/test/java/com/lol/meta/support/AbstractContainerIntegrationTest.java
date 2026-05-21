package com.lol.meta.support;

import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.test.context.ActiveProfiles;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.testcontainers.containers.GenericContainer;
import org.testcontainers.containers.MySQLContainer;
import org.testcontainers.utility.DockerImageName;

@SpringBootTest
@ActiveProfiles("test")
public abstract class AbstractContainerIntegrationTest {

  private static final DockerImageName MYSQL_IMAGE = DockerImageName.parse("mysql:8.4.0");
  private static final DockerImageName REDIS_IMAGE = DockerImageName.parse("redis:7.4-alpine");

  private static final MySQLContainer<?> MYSQL =
      new MySQLContainer<>(MYSQL_IMAGE)
          .withDatabaseName("loot_of_legends_meta")
          .withUsername("meta_test")
          .withPassword("meta_test");

  private static final GenericContainer<?> REDIS =
      new GenericContainer<>(REDIS_IMAGE).withExposedPorts(6379);

  static {
    MYSQL.start();
    REDIS.start();
  }

  @DynamicPropertySource
  static void registerContainerProperties(DynamicPropertyRegistry registry) {
    registry.add("spring.datasource.url", MYSQL::getJdbcUrl);
    registry.add("spring.datasource.username", MYSQL::getUsername);
    registry.add("spring.datasource.password", MYSQL::getPassword);
    registry.add("spring.datasource.driver-class-name", MYSQL::getDriverClassName);
    registry.add("spring.data.redis.host", REDIS::getHost);
    registry.add("spring.data.redis.port", () -> REDIS.getMappedPort(6379));
  }
}
