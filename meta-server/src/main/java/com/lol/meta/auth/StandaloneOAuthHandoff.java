package com.lol.meta.auth;

import java.time.Instant;

public record StandaloneOAuthHandoff(long accountId, String state, Instant expiresAt) {}
