package com.lol.meta.firewall;

import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotBlank;
import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.validation.annotation.Validated;

@Validated
@ConfigurationProperties(prefix = "release0.firewall")
public record GameFirewallProperties(
    boolean enabled,
    @NotBlank String socketPath,
    @Min(5) @Max(120) int preAuthTtlSeconds,
    @Min(5) @Max(1800) int activeSessionTtlSeconds,
    @NotBlank String trustedForwardedHeader,
    boolean trustForwardedHeader) {}
