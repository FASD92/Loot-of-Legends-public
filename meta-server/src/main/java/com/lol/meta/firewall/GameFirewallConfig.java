package com.lol.meta.firewall;

import java.nio.file.Path;
import java.time.Duration;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@Configuration
@EnableConfigurationProperties(GameFirewallProperties.class)
public class GameFirewallConfig {

  private static final Duration AGENT_CONNECT_TIMEOUT = Duration.ofMillis(500);
  private static final Duration AGENT_READ_TIMEOUT = Duration.ofMillis(500);

  @Bean
  public GameFirewallClient gameFirewallClient(GameFirewallProperties properties) {
    if (!properties.enabled()) {
      return new NoopGameFirewallClient();
    }
    return new UnixSocketGameFirewallClient(
        Path.of(properties.socketPath()), AGENT_CONNECT_TIMEOUT, AGENT_READ_TIMEOUT);
  }

  @Bean
  public TrustedClientAddressResolver trustedClientAddressResolver(
      GameFirewallProperties properties) {
    return new TrustedClientAddressResolver(properties);
  }
}
