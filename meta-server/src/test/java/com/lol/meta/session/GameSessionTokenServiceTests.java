package com.lol.meta.session;

import static org.assertj.core.api.Assertions.assertThat;

import com.lol.meta.firewall.GameFirewallClient;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import org.junit.jupiter.api.Test;

class GameSessionTokenServiceTests {
  @Test
  void claimConsumesTokenAndCreatesActiveSession() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putReservation("token-1", 123L, "player123", Instant.ofEpochMilli(11_000));
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(5_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.ACCEPTED);
    assertThat(response.accountId()).isEqualTo(123L);
    assertThat(response.nickname()).isEqualTo("player123");
    assertThat(repository.hasReservation("token-1")).isFalse();
    assertThat(repository.hasActiveSession(123L)).isTrue();
  }

  @Test
  void duplicateClaimIsRejectedAfterTokenWasConsumed() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putReservation("token-1", 123L, "player123", Instant.ofEpochMilli(11_000));
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    service.claim(
        new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(5_000));
    GameSessionClaimResponse second =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-2"), Instant.ofEpochMilli(6_000));

    assertThat(second.status()).isEqualTo(GameSessionClaimStatus.REJECTED);
  }

  @Test
  void missingReservationClaimReleasesReservedCapacityMember() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putLeakedReservationCapacity("missing-token");
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("missing-token", "client-1"),
            Instant.ofEpochMilli(12_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.REJECTED);
    assertThat(repository.hasReservedCapacity("missing-token")).isFalse();
  }

  @Test
  void claimSuccessClearsPendingReservationIndex() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putReservation("token-1", 123L, "player123", Instant.ofEpochMilli(11_000));
    repository.putPendingReservationIndex(123L, "token-1");
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(5_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.ACCEPTED);
    assertThat(repository.pendingReservationToken(123L)).isNull();
  }

  @Test
  void expiredClaimClearsPendingReservationIndex() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putReservation("token-1", 123L, "player123", Instant.ofEpochMilli(11_000));
    repository.putPendingReservationIndex(123L, "token-1");
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(12_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.REJECTED);
    assertThat(repository.pendingReservationToken(123L)).isNull();
  }

  @Test
  void claimAtExactReservationExpirationIsRejected() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putReservation("token-1", 123L, "player123", Instant.ofEpochMilli(11_000));
    repository.putPendingReservationIndex(123L, "token-1");
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(11_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.REJECTED);
    assertThat(repository.pendingReservationToken(123L)).isNull();
    assertThat(repository.hasActiveSession(123L)).isFalse();
  }

  @Test
  void staleReplacementClaimAfterOldSessionReleaseIsRejectedWithoutGrowingActiveMembers() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putActiveSession(123L, "active-session-1");
    repository.putReplacementReservation(
        "token-1", 123L, "player123", Instant.ofEpochMilli(11_000), "active-session-1");
    repository.putPendingReservationIndex(123L, "token-1");
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    service.release(new GameSessionReleaseRequest(123L, "active-session-1"));
    repository.putActiveSession(456L, "other-session-1");
    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(5_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.REJECTED);
    assertThat(repository.hasActiveSession(123L)).isFalse();
    assertThat(repository.hasReservation("token-1")).isFalse();
    assertThat(repository.pendingReservationToken(123L)).isNull();
    assertThat(repository.activeSessionMemberCount()).isEqualTo(1);
    assertThat(repository.hasActiveSessionMember("other-session-1")).isTrue();
  }

  @Test
  void validReplacementClaimSwapsOldSessionWithoutGrowingActiveMembers() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putActiveSession(123L, "active-session-1");
    repository.putReplacementReservation(
        "token-1", 123L, "player123", Instant.ofEpochMilli(11_000), "active-session-1");
    repository.putPendingReservationIndex(123L, "token-1");
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(5_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.ACCEPTED);
    assertThat(response.replacedSessionId()).isEqualTo("active-session-1");
    assertThat(repository.hasActiveSessionMember("active-session-1")).isFalse();
    assertThat(repository.hasActiveSessionMember("client-1")).isTrue();
    assertThat(repository.activeSessionMemberCount()).isEqualTo(1);
  }

  @Test
  void replacementClaimIsRejectedWhenStoredReplacementIsNoLongerCurrent() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putActiveSession(123L, "different-session");
    repository.putReplacementReservation(
        "token-1", 123L, "player123", Instant.ofEpochMilli(11_000), "active-session-1");
    repository.putPendingReservationIndex(123L, "token-1");
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(5_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.REJECTED);
    assertThat(repository.hasActiveSessionMember("different-session")).isTrue();
    assertThat(repository.hasActiveSessionMember("client-1")).isFalse();
    assertThat(repository.hasReservation("token-1")).isFalse();
    assertThat(repository.pendingReservationToken(123L)).isNull();
  }

  @Test
  void releaseActiveSessionClearsMatchingPendingReplacementReservation() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    repository.putActiveSession(123L, "active-session-1");
    repository.putReplacementReservation(
        "token-1", 123L, "player123", Instant.ofEpochMilli(11_000), "active-session-1");
    repository.putPendingReservationIndex(123L, "token-1");
    GameSessionTokenService service = GameSessionTokenService.forTest(repository);

    boolean released = service.release(new GameSessionReleaseRequest(123L, "active-session-1"));

    assertThat(released).isTrue();
    assertThat(repository.hasReservation("token-1")).isFalse();
    assertThat(repository.pendingReservationToken(123L)).isNull();
    assertThat(repository.activeSessionMemberCount()).isZero();
  }

  @Test
  void acceptedClaimRenewsActiveFirewallSessionForReservationClientIp() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    repository.putReservation(
        "token-1", 123L, "player123", Instant.ofEpochMilli(11_000), "203.0.113.41");
    GameSessionTokenService service =
        GameSessionTokenService.forTest(repository, firewallClient, Duration.ofSeconds(45));

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("token-1", "client-1"), Instant.ofEpochMilli(5_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.ACCEPTED);
    assertThat(firewallClient.renewCalls)
        .containsExactly(
            new FirewallRenewCall("203.0.113.41", Duration.ofSeconds(45), "client-1"));
  }

  @Test
  void rejectedClaimDoesNotRenewActiveFirewallSession() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    GameSessionTokenService service =
        GameSessionTokenService.forTest(repository, firewallClient, Duration.ofSeconds(45));

    GameSessionClaimResponse response =
        service.claim(
            new GameSessionClaimRequest("missing-token", "client-1"),
            Instant.ofEpochMilli(5_000));

    assertThat(response.status()).isEqualTo(GameSessionClaimStatus.REJECTED);
    assertThat(firewallClient.renewCalls).isEmpty();
  }

  @Test
  void successfulReleaseClosesFirewallSession() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    repository.putActiveSession(123L, "client-1");
    GameSessionTokenService service =
        GameSessionTokenService.forTest(repository, firewallClient, Duration.ofSeconds(45));

    boolean released = service.release(new GameSessionReleaseRequest(123L, "client-1"));

    assertThat(released).isTrue();
    assertThat(firewallClient.closeSessionCalls).containsExactly("client-1");
  }

  @Test
  void firewallCloseFailureDoesNotResurrectReleasedSession() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.closeFailure();
    repository.putActiveSession(123L, "client-1");
    GameSessionTokenService service =
        GameSessionTokenService.forTest(repository, firewallClient, Duration.ofSeconds(45));

    boolean released = service.release(new GameSessionReleaseRequest(123L, "client-1"));

    assertThat(released).isTrue();
    assertThat(repository.hasActiveSession(123L)).isFalse();
    assertThat(firewallClient.closeSessionCalls).containsExactly("client-1");
  }

  @Test
  void renewActiveSessionUsesStoredClientIpAndActiveFirewallTtl() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    repository.putActiveSession(123L, "client-1", "203.0.113.41");
    GameSessionTokenService service =
        GameSessionTokenService.forTest(repository, firewallClient, Duration.ofSeconds(45));

    boolean renewed = service.renew(new GameSessionRenewRequest(123L, "client-1"));

    assertThat(renewed).isTrue();
    assertThat(firewallClient.renewCalls)
        .containsExactly(
            new FirewallRenewCall("203.0.113.41", Duration.ofSeconds(45), "client-1"));
  }

  @Test
  void renewRejectsMismatchedActiveSessionWithoutFirewallCall() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    repository.putActiveSession(123L, "client-1", "203.0.113.41");
    GameSessionTokenService service =
        GameSessionTokenService.forTest(repository, firewallClient, Duration.ofSeconds(45));

    boolean renewed = service.renew(new GameSessionRenewRequest(123L, "other-client"));

    assertThat(renewed).isFalse();
    assertThat(firewallClient.renewCalls).isEmpty();
  }

  @Test
  void releaseClearsActiveSessionMetadataForLaterRenew() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    repository.putActiveSession(123L, "client-1", "203.0.113.41");
    GameSessionTokenService service =
        GameSessionTokenService.forTest(repository, firewallClient, Duration.ofSeconds(45));

    assertThat(service.release(new GameSessionReleaseRequest(123L, "client-1"))).isTrue();
    boolean renewed = service.renew(new GameSessionRenewRequest(123L, "client-1"));

    assertThat(renewed).isFalse();
    assertThat(firewallClient.renewCalls).isEmpty();
  }

  @Test
  void renewReturnsFalseWhenFirewallRenewFailsWithoutRemovingActiveSession() {
    FakeGameSessionTokenRepository repository = new FakeGameSessionTokenRepository();
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.renewFailure();
    repository.putActiveSession(123L, "client-1", "203.0.113.41");
    GameSessionTokenService service =
        GameSessionTokenService.forTest(repository, firewallClient, Duration.ofSeconds(45));

    boolean renewed = service.renew(new GameSessionRenewRequest(123L, "client-1"));

    assertThat(renewed).isFalse();
    assertThat(repository.hasActiveSession(123L)).isTrue();
    assertThat(firewallClient.renewCalls)
        .containsExactly(
            new FirewallRenewCall("203.0.113.41", Duration.ofSeconds(45), "client-1"));
  }

  private record FirewallRenewCall(String clientIp, Duration ttl, String sessionId) {}

  private static final class RecordingGameFirewallClient implements GameFirewallClient {
    private final List<FirewallRenewCall> renewCalls = new ArrayList<>();
    private final List<String> closeSessionCalls = new ArrayList<>();
    private final boolean renewSuccess;
    private final boolean closeSuccess;

    private RecordingGameFirewallClient(boolean renewSuccess, boolean closeSuccess) {
      this.renewSuccess = renewSuccess;
      this.closeSuccess = closeSuccess;
    }

    static RecordingGameFirewallClient success() {
      return new RecordingGameFirewallClient(true, true);
    }

    static RecordingGameFirewallClient renewFailure() {
      return new RecordingGameFirewallClient(false, true);
    }

    static RecordingGameFirewallClient closeFailure() {
      return new RecordingGameFirewallClient(true, false);
    }

    @Override
    public Decision allowPreAuth(String clientIp, Duration ttl, String reason) {
      return Decision.ok();
    }

    @Override
    public Decision renewActiveSession(String clientIp, Duration ttl, String sessionId) {
      renewCalls.add(new FirewallRenewCall(clientIp, ttl, sessionId));
      if (renewSuccess) {
        return Decision.ok();
      }
      return Decision.failure("FirewallAgentUnavailable", "firewall agent unavailable");
    }

    @Override
    public Decision closeClientIp(String clientIp) {
      return Decision.ok();
    }

    @Override
    public Decision closeSession(String sessionId) {
      closeSessionCalls.add(sessionId);
      if (closeSuccess) {
        return Decision.ok();
      }
      return Decision.failure("FirewallAgentUnavailable", "firewall agent unavailable");
    }
  }

  private static final class FakeGameSessionTokenRepository
      implements GameSessionTokenRepository {

    private final Map<String, GameSessionReservation> reservations = new HashMap<>();
    private final Map<Long, String> activeSessions = new HashMap<>();
    private final Map<String, ActiveGameSession> activeSessionMetadata = new HashMap<>();
    private final Map<Long, String> pendingReservations = new HashMap<>();
    private final Set<String> activeSessionMembers = new HashSet<>();

    void putReservation(String token, long accountId, String nickname, Instant expiresAt) {
      putReservation(token, accountId, nickname, expiresAt, "127.0.0.1");
    }

    void putReservation(
        String token, long accountId, String nickname, Instant expiresAt, String clientIp) {
      reservations.put(
          token, new GameSessionReservation(accountId, nickname, expiresAt, null, clientIp));
      activeSessionMembers.add(reservationMember(token));
    }

    void putReplacementReservation(
        String token, long accountId, String nickname, Instant expiresAt, String replacedSessionId) {
      reservations.put(
          token,
          new GameSessionReservation(
              accountId, nickname, expiresAt, replacedSessionId, "127.0.0.1"));
    }

    void putActiveSession(long accountId, String connectionId) {
      putActiveSession(accountId, connectionId, "127.0.0.1");
    }

    void putActiveSession(long accountId, String connectionId, String clientIp) {
      activeSessions.put(accountId, connectionId);
      activeSessionMetadata.put(
          connectionId, new ActiveGameSession(accountId, connectionId, clientIp));
      activeSessionMembers.add(connectionId);
    }

    void putLeakedReservationCapacity(String token) {
      activeSessionMembers.add(reservationMember(token));
    }

    void putPendingReservationIndex(long accountId, String token) {
      pendingReservations.put(accountId, token);
    }

    boolean hasReservation(String token) {
      return reservations.containsKey(token);
    }

    boolean hasActiveSession(long accountId) {
      return activeSessions.containsKey(accountId);
    }

    boolean hasReservedCapacity(String token) {
      return activeSessionMembers.contains(reservationMember(token));
    }

    boolean hasActiveSessionMember(String connectionId) {
      return activeSessionMembers.contains(connectionId);
    }

    int activeSessionMemberCount() {
      return activeSessionMembers.size();
    }

    String pendingReservationToken(long accountId) {
      return pendingReservations.get(accountId);
    }

    @Override
    public Optional<ClaimedGameSession> claimReservation(
        String gameSessionToken, String connectionId, Instant now) {
      GameSessionReservation reservation = reservations.get(gameSessionToken);
      if (reservation == null || !now.isBefore(reservation.reservationExpiresAt())) {
        activeSessionMembers.remove(reservationMember(gameSessionToken));
        reservations.remove(gameSessionToken);
        if (reservation != null) {
          pendingReservations.remove(reservation.accountId(), gameSessionToken);
        }
        return Optional.empty();
      }
      reservations.remove(gameSessionToken);
      activeSessionMembers.remove(reservationMember(gameSessionToken));
      pendingReservations.remove(reservation.accountId(), gameSessionToken);
      String replacedSessionId = reservation.replacedSessionId();
      if (replacedSessionId != null
          && !replacedSessionId.equals(activeSessions.get(reservation.accountId()))) {
        return Optional.empty();
      }
      if (replacedSessionId != null) {
        activeSessions.remove(reservation.accountId(), replacedSessionId);
        activeSessionMetadata.remove(replacedSessionId);
        activeSessionMembers.remove(replacedSessionId);
      } else {
        replacedSessionId = activeSessions.put(reservation.accountId(), connectionId);
        if (replacedSessionId != null) {
          activeSessionMetadata.remove(replacedSessionId);
          activeSessionMembers.remove(replacedSessionId);
        }
      }
      activeSessions.put(reservation.accountId(), connectionId);
      activeSessionMetadata.put(
          connectionId,
          new ActiveGameSession(
              reservation.accountId(), connectionId, reservation.clientIp()));
      activeSessionMembers.add(connectionId);
      return Optional.of(
          new ClaimedGameSession(
              reservation.accountId(),
              reservation.nickname(),
              replacedSessionId,
              reservation.reservationExpiresAt(),
              reservation.clientIp()));
    }

    @Override
    public boolean releaseActiveSession(long accountId, String connectionId) {
      boolean removed = activeSessions.remove(accountId, connectionId);
      if (removed) {
        activeSessionMetadata.remove(connectionId);
        activeSessionMembers.remove(connectionId);
        clearPendingReplacementForReleasedSession(accountId, connectionId);
      }
      return removed;
    }

    @Override
    public Optional<ActiveGameSession> findActiveSession(long accountId, String connectionId) {
      if (!connectionId.equals(activeSessions.get(accountId))) {
        return Optional.empty();
      }
      return Optional.ofNullable(activeSessionMetadata.get(connectionId));
    }

    private void clearPendingReplacementForReleasedSession(long accountId, String connectionId) {
      String token = pendingReservations.get(accountId);
      if (token == null) {
        return;
      }

      GameSessionReservation reservation = reservations.get(token);
      if (reservation != null && connectionId.equals(reservation.replacedSessionId())) {
        reservations.remove(token);
        pendingReservations.remove(accountId, token);
        activeSessionMembers.remove(reservationMember(token));
      }
    }

    private String reservationMember(String token) {
      return "reservation:" + token;
    }
  }
}
