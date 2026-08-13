package com.fasd92.lootoflegends.meta.platform.security;

import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.security.config.annotation.web.builders.HttpSecurity;
import org.springframework.security.config.http.SessionCreationPolicy;
import org.springframework.security.web.SecurityFilterChain;
import org.springframework.security.web.access.intercept.AuthorizationFilter;

@Configuration
public class MetaSecurityConfiguration {
  @Bean
  SecurityFilterChain metaSecurity(
      HttpSecurity http,
      @Value("${loot.internal.service-token}") String serviceToken,
      DesktopAuthUseCase desktopAuth)
      throws Exception {
    var internalAuthentication = new InternalServiceAuthenticationFilter(serviceToken);
    var metaSessionAuthentication = new MetaSessionAuthenticationFilter(desktopAuth);
    return http.csrf(
            csrf ->
                csrf.ignoringRequestMatchers(
                    "/internal/**",
                    "/api/v1/desktop-auth/**",
                    "/api/v1/game-credentials",
                    "/private/**"))
        .sessionManagement(
            session -> session.sessionCreationPolicy(SessionCreationPolicy.STATELESS))
        .httpBasic(basic -> basic.disable())
        .formLogin(form -> form.disable())
        .logout(logout -> logout.disable())
        .requestCache(cache -> cache.disable())
        .exceptionHandling(
            errors ->
                errors.authenticationEntryPoint(
                    (request, response, exception) ->
                        InternalServiceAuthenticationFilter.unauthorized(response)))
        .authorizeHttpRequests(
            requests ->
                requests
                    .requestMatchers("/internal/**")
                    .hasRole("GAME_SERVER")
                    .requestMatchers("/api/v1/desktop-auth/**", "/v1/identity/google/callback")
                    .permitAll()
                    .requestMatchers("/api/**")
                    .authenticated()
                    .anyRequest()
                    .permitAll())
        .addFilterBefore(internalAuthentication, AuthorizationFilter.class)
        .addFilterBefore(metaSessionAuthentication, AuthorizationFilter.class)
        .build();
  }
}
