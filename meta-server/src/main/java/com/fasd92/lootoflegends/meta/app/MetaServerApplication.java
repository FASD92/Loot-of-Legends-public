package com.fasd92.lootoflegends.meta.app;

import java.security.SecureRandom;
import java.time.Clock;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.autoconfigure.security.servlet.UserDetailsServiceAutoConfiguration;
import org.springframework.context.annotation.Bean;
import org.springframework.scheduling.annotation.EnableScheduling;

@EnableScheduling
@SpringBootApplication(
    scanBasePackages = "com.fasd92.lootoflegends.meta",
    exclude = UserDetailsServiceAutoConfiguration.class)
public class MetaServerApplication {
  public static void main(String[] args) {
    SpringApplication.run(MetaServerApplication.class, args);
  }

  @Bean
  Clock clock() {
    return Clock.systemUTC();
  }

  @Bean
  SecureRandom secureRandom() {
    return new SecureRandom();
  }
}
