package com.lol.meta.auth;

import org.springframework.beans.factory.ObjectProvider;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.http.HttpMethod;
import org.springframework.http.HttpStatus;
import org.springframework.security.config.annotation.web.builders.HttpSecurity;
import org.springframework.security.config.annotation.web.configuration.EnableWebSecurity;
import org.springframework.security.config.annotation.web.configurers.AbstractHttpConfigurer;
import org.springframework.security.oauth2.client.registration.ClientRegistrationRepository;
import org.springframework.security.web.authentication.HttpStatusEntryPoint;
import org.springframework.security.web.SecurityFilterChain;

@Configuration
@EnableWebSecurity
public class Release0SecurityConfig {

  @Bean
  SecurityFilterChain release0SecurityFilterChain(
      HttpSecurity http,
      ObjectProvider<ClientRegistrationRepository> clientRegistrationRepository,
      ObjectProvider<Release0OAuth2UserService> oauth2UserService,
      ObjectProvider<Release0OidcUserService> oidcUserService)
      throws Exception {
    boolean oauthEnabled = clientRegistrationRepository.getIfAvailable() != null;
    http.csrf(
            csrf ->
                csrf.ignoringRequestMatchers(
                    "/internal/**", "/api/release0/auth/standalone/exchange"))
        .httpBasic(AbstractHttpConfigurer::disable)
        .formLogin(AbstractHttpConfigurer::disable)
        .logout(AbstractHttpConfigurer::disable)
        .exceptionHandling(
            exception -> exception.authenticationEntryPoint(new HttpStatusEntryPoint(HttpStatus.FORBIDDEN)))
        .authorizeHttpRequests(
            authorize -> {
              authorize
                  .requestMatchers(HttpMethod.GET, "/api/release0/auth/session")
                  .permitAll()
                  .requestMatchers(HttpMethod.GET, "/api/release0/auth/csrf")
                  .permitAll()
                  .requestMatchers(HttpMethod.GET, "/api/release0/auth/standalone/start")
                  .permitAll()
                  .requestMatchers(HttpMethod.GET, "/api/release0/auth/standalone/complete")
                  .permitAll()
                  .requestMatchers(HttpMethod.POST, "/api/release0/auth/standalone/exchange")
                  .permitAll();
              if (oauthEnabled) {
                authorize
                    .requestMatchers(HttpMethod.GET, "/oauth2/authorization/*")
                    .permitAll()
                    .requestMatchers(HttpMethod.GET, "/login/oauth2/code/*")
                    .permitAll();
              }
              authorize
                  .requestMatchers(HttpMethod.POST, "/api/release0/auth/nickname")
                  .permitAll()
                  .requestMatchers(HttpMethod.POST, "/api/release0/auth/nickname/check")
                  .permitAll()
                  .requestMatchers(HttpMethod.POST, "/api/release0/admission/enter")
                  .permitAll()
                  .requestMatchers(HttpMethod.GET, "/api/release0/admission/status")
                  .permitAll()
                  .requestMatchers(HttpMethod.GET, "/api/release0/admission/queue/*")
                  .permitAll()
                  .requestMatchers(HttpMethod.DELETE, "/api/release0/admission/queue")
                  .permitAll()
                  .requestMatchers(HttpMethod.DELETE, "/api/release0/admission/queue/*")
                  .permitAll()
                  .requestMatchers(
                      HttpMethod.GET, "/actuator/health", "/actuator/health/**", "/actuator/info")
                  .permitAll()
                  .requestMatchers("/internal/**")
                  .permitAll()
                  .anyRequest()
                  .denyAll();
            });
    if (oauthEnabled) {
      http.oauth2Login(
          oauth2 ->
              oauth2
                  .loginPage("/oauth2/authorization/google")
                  .defaultSuccessUrl("/api/release0/auth/standalone/complete", true)
                  .userInfoEndpoint(
                      userInfo -> {
                        Release0OAuth2UserService userService = oauth2UserService.getIfAvailable();
                        if (userService != null) {
                          userInfo.userService(userService);
                        }
                        Release0OidcUserService oidcService = oidcUserService.getIfAvailable();
                        if (oidcService != null) {
                          userInfo.oidcUserService(oidcService);
                        }
                      }));
    }
    return http.build();
  }
}
