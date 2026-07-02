package com.lol.meta.firewall;

import java.time.Duration;

public final class NoopGameFirewallClient implements GameFirewallClient {

  @Override
  public Decision allowPreAuth(String clientIp, Duration ttl, String reason) {
    return Decision.ok();
  }

  @Override
  public Decision renewActiveSession(String clientIp, Duration ttl, String sessionId) {
    return Decision.ok();
  }

  @Override
  public Decision closeClientIp(String clientIp) {
    return Decision.ok();
  }

  @Override
  public Decision closeSession(String sessionId) {
    return Decision.ok();
  }
}
