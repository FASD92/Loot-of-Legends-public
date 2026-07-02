package com.lol.meta.firewall;

import java.time.Duration;

public interface GameFirewallClient {

  Decision allowPreAuth(String clientIp, Duration ttl, String reason);

  Decision renewActiveSession(String clientIp, Duration ttl, String sessionId);

  Decision closeClientIp(String clientIp);

  Decision closeSession(String sessionId);

  record Decision(boolean success, String errorCode, String message) {
    public Decision {
      errorCode = errorCode == null ? "" : errorCode;
      message = message == null ? "" : message;
    }

    public static Decision ok() {
      return new Decision(true, "", "");
    }

    public static Decision failure(String errorCode, String message) {
      return new Decision(false, errorCode, message);
    }
  }
}
