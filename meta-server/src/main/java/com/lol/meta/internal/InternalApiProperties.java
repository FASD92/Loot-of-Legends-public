package com.lol.meta.internal;

import jakarta.validation.constraints.NotBlank;
import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.validation.annotation.Validated;

@Validated
@ConfigurationProperties(prefix = "meta.internal")
public record InternalApiProperties(@NotBlank String token) {}
