package com.lol.meta.internal;

import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Configuration;
import org.springframework.web.servlet.config.annotation.InterceptorRegistry;
import org.springframework.web.servlet.config.annotation.WebMvcConfigurer;

@Configuration
@EnableConfigurationProperties(InternalApiProperties.class)
public class InternalWebMvcConfig implements WebMvcConfigurer {

  private final InternalTokenInterceptor internalTokenInterceptor;

  public InternalWebMvcConfig(InternalTokenInterceptor internalTokenInterceptor) {
    this.internalTokenInterceptor = internalTokenInterceptor;
  }

  @Override
  public void addInterceptors(InterceptorRegistry registry) {
    registry.addInterceptor(internalTokenInterceptor).addPathPatterns("/internal/**");
  }
}
