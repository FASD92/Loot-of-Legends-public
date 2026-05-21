package com.lol.meta;

import static org.assertj.core.api.Assertions.assertThat;

import com.lol.meta.support.AbstractContainerIntegrationTest;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.data.redis.core.RedisCallback;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.jdbc.core.JdbcTemplate;

class ContainerConnectionTests extends AbstractContainerIntegrationTest {

  @Autowired private JdbcTemplate jdbcTemplate;

  @Autowired private StringRedisTemplate redisTemplate;

  @Test
  void mysqlConnectionRespondsToSimpleQuery() {
    Integer result = jdbcTemplate.queryForObject("SELECT 1", Integer.class);

    assertThat(result).isEqualTo(1);
  }

  @Test
  void redisConnectionRespondsToPing() {
    String response = redisTemplate.execute((RedisCallback<String>) connection -> connection.ping());

    assertThat(response).isEqualTo("PONG");
  }
}
