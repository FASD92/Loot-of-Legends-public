package com.lol.meta.session;

public record GameSessionClaimRequest(String gameSessionToken, String connectionId) {}
