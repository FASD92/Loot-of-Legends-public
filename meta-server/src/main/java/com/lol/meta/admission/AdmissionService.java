package com.lol.meta.admission;

import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.annotation.JsonValue;
import com.lol.meta.firewall.GameFirewallClient;
import com.lol.meta.firewall.NoopGameFirewallClient;
import java.security.SecureRandom;
import java.time.Duration;
import java.time.Instant;
import java.util.Base64;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;
import org.springframework.stereotype.Service;

@Service
public class AdmissionService {

  private static final int TOKEN_BYTES = 32;
  private static final int DEFAULT_ACTIVE_SESSION_CAPACITY = 100;
  private static final Duration DEFAULT_QUEUE_ENTRY_TTL = Duration.ofSeconds(30);
  private static final Duration DEFAULT_RESERVATION_TTL = Duration.ofSeconds(10);
  private static final Duration DEFAULT_PRE_AUTH_FIREWALL_TTL = Duration.ofSeconds(60);

  private final AdmissionQueueRepository repository;
  private final AdmissionNicknameProvider nicknameProvider;
  private final GameServerEndpoint gameServerEndpoint;
  private final int activeSessionCapacity;
  private final Duration queueEntryTtl;
  private final Duration reservationTtl;
  private final SecureRandom secureRandom;
  private final GameFirewallClient firewallClient;
  private final Duration preAuthFirewallTtl;

  @Autowired
  public AdmissionService(
      AdmissionQueueRepository repository,
      AdmissionNicknameProvider nicknameProvider,
      @Value("${release0.game-server.host}") String gameServerHost,
      @Value("${release0.game-server.tcp-port}") int gameServerTcpPort,
      @Value("${release0.game-server.rudp-port}") int gameServerRudpPort,
      @Value("${release0.admission.active-session-capacity}") int activeSessionCapacity,
      @Value("${release0.admission.queue-entry-ttl-seconds}") long queueEntryTtlSeconds,
      @Value("${release0.admission.reservation-ttl-seconds}") long reservationTtlSeconds,
      @Value("${release0.firewall.pre-auth-ttl-seconds}") long preAuthFirewallTtlSeconds,
      GameFirewallClient firewallClient) {
    this(
        repository,
        nicknameProvider,
        gameServerHost,
        gameServerTcpPort,
        gameServerRudpPort,
        activeSessionCapacity,
        Duration.ofSeconds(queueEntryTtlSeconds),
        Duration.ofSeconds(reservationTtlSeconds),
        new SecureRandom(),
        firewallClient,
        Duration.ofSeconds(preAuthFirewallTtlSeconds));
  }

  private AdmissionService(
      AdmissionQueueRepository repository,
      AdmissionNicknameProvider nicknameProvider,
      String gameServerHost,
      int gameServerTcpPort,
      int gameServerRudpPort,
      int activeSessionCapacity,
      Duration queueEntryTtl,
      Duration reservationTtl,
      SecureRandom secureRandom,
      GameFirewallClient firewallClient,
      Duration preAuthFirewallTtl) {
    if (activeSessionCapacity < 1) {
      throw new IllegalArgumentException("activeSessionCapacity must be positive");
    }
    this.repository = Objects.requireNonNull(repository, "repository");
    this.nicknameProvider = Objects.requireNonNull(nicknameProvider, "nicknameProvider");
    this.gameServerEndpoint =
        new GameServerEndpoint(gameServerHost, gameServerTcpPort, gameServerRudpPort);
    this.activeSessionCapacity = activeSessionCapacity;
    this.queueEntryTtl = Objects.requireNonNull(queueEntryTtl, "queueEntryTtl");
    this.reservationTtl = Objects.requireNonNull(reservationTtl, "reservationTtl");
    this.secureRandom = Objects.requireNonNull(secureRandom, "secureRandom");
    this.firewallClient = Objects.requireNonNull(firewallClient, "firewallClient");
    this.preAuthFirewallTtl =
        Objects.requireNonNull(preAuthFirewallTtl, "preAuthFirewallTtl");
  }

  public static AdmissionService forTest(
      AdmissionQueueRepository repository,
      String gameServerHost,
      int gameServerTcpPort,
      int gameServerRudpPort) {
    return new AdmissionService(
        repository,
        accountId -> "player" + accountId,
        gameServerHost,
        gameServerTcpPort,
        gameServerRudpPort,
        DEFAULT_ACTIVE_SESSION_CAPACITY,
        DEFAULT_QUEUE_ENTRY_TTL,
        DEFAULT_RESERVATION_TTL,
        new SecureRandom(),
        new NoopGameFirewallClient(),
        DEFAULT_PRE_AUTH_FIREWALL_TTL);
  }

  public static AdmissionService forTest(
      AdmissionQueueRepository repository,
      String gameServerHost,
      int gameServerTcpPort,
      int gameServerRudpPort,
      GameFirewallClient firewallClient) {
    return new AdmissionService(
        repository,
        accountId -> "player" + accountId,
        gameServerHost,
        gameServerTcpPort,
        gameServerRudpPort,
        DEFAULT_ACTIVE_SESSION_CAPACITY,
        DEFAULT_QUEUE_ENTRY_TTL,
        DEFAULT_RESERVATION_TTL,
        new SecureRandom(),
        firewallClient,
        DEFAULT_PRE_AUTH_FIREWALL_TTL);
  }

  static AdmissionService forTest(
      AdmissionQueueRepository repository,
      String gameServerHost,
      int gameServerTcpPort,
      int gameServerRudpPort,
      AdmissionNicknameProvider nicknameProvider) {
    return new AdmissionService(
        repository,
        nicknameProvider,
        gameServerHost,
        gameServerTcpPort,
        gameServerRudpPort,
        DEFAULT_ACTIVE_SESSION_CAPACITY,
        DEFAULT_QUEUE_ENTRY_TTL,
        DEFAULT_RESERVATION_TTL,
        new SecureRandom(),
        new NoopGameFirewallClient(),
        DEFAULT_PRE_AUTH_FIREWALL_TTL);
  }

  public AdmissionResponse enter(long accountId, Instant now) {
    return enter(accountId, "127.0.0.1", now);
  }

  public AdmissionResponse enter(long accountId, String clientIp, Instant now) {
    validateAccountId(accountId);
    Objects.requireNonNull(now, "now");

    Optional<AdmissionReservation> pendingReservation =
        repository.findPendingReservation(accountId, now);
    if (pendingReservation.isPresent()) {
      return admitted(pendingReservation.get(), clientIp);
    }

    if (repository.hasActiveSession(accountId)) {
      return admitReplacement(accountId, clientIp, now);
    }

    Optional<AdmissionQueuePosition> pendingQueue =
        repository.findPendingQueuePosition(accountId, now);
    if (pendingQueue.isPresent()) {
      AdmissionQueuePosition position = pendingQueue.get();
      return AdmissionResponse.queued(position.position(), position.queueToken());
    }

    AdmissionReservation reservation = newReservation(accountId, clientIp, now, null);
    AdmissionReservationCreation creation =
        repository.createReservationIfCapacityAvailable(
            reservation, activeSessionCapacity, now);
    if (creation.status() != AdmissionReservationCreationStatus.CAPACITY_FULL) {
      return admitted(creation.reservation(), clientIp);
    }

    String queueToken = newToken();
    AdmissionQueuePosition position =
        repository.enqueue(
            new AdmissionQueueEntry(queueToken, accountId, now.plus(queueEntryTtl)), now);
    return AdmissionResponse.queued(position.position(), position.queueToken());
  }

  public AdmissionResponse status(long accountId, Instant now) {
    return status(accountId, "127.0.0.1", now);
  }

  public AdmissionResponse status(long accountId, String clientIp, Instant now) {
    validateAccountId(accountId);
    Objects.requireNonNull(now, "now");

    return repository
        .findQueuePosition(accountId, now)
        .map(position -> statusForQueuedAccount(accountId, position, clientIp, now))
        .orElseGet(AdmissionResponse::notQueued);
  }

  public AdmissionResponse status(String queueToken, Instant now) {
    return status(queueToken, "127.0.0.1", now);
  }

  public AdmissionResponse status(String queueToken, String clientIp, Instant now) {
    Objects.requireNonNull(now, "now");
    if (queueToken == null || queueToken.isBlank()) {
      return AdmissionResponse.notQueued();
    }

    AdmissionQueueLookup lookup = repository.findQueuePosition(queueToken.trim(), now);
    return switch (lookup.status()) {
      case QUEUED -> statusForQueuedAccount(lookup.accountId(), lookup.position(), clientIp, now);
      case EXPIRED -> AdmissionResponse.queueExpired();
      case NOT_QUEUED -> AdmissionResponse.notQueued();
    };
  }

  public AdmissionResponse cancel(long accountId, Instant now) {
    validateAccountId(accountId);
    Objects.requireNonNull(now, "now");

    return repository
        .findQueuePosition(accountId, now)
        .map(
            position -> {
              repository.cancelQueueEntry(position.queueToken());
              return AdmissionResponse.notQueued();
            })
        .orElseGet(AdmissionResponse::notQueued);
  }

  public AdmissionResponse cancel(String queueToken, Instant now) {
    Objects.requireNonNull(now, "now");
    if (queueToken == null || queueToken.isBlank()) {
      return AdmissionResponse.notQueued();
    }
    repository.cancelQueueEntry(queueToken.trim());
    return AdmissionResponse.notQueued();
  }

  private AdmissionResponse statusForQueuedAccount(
      long accountId, AdmissionQueuePosition position, String clientIp, Instant now) {
    if (position.position() == 1) {
      AdmissionReservation reservation = newReservation(accountId, clientIp, now, null);
      if (repository.promoteQueuedHeadIfCapacityAvailable(
          position.queueToken(), reservation, activeSessionCapacity, now)) {
        return admitted(reservation, clientIp);
      }

      AdmissionQueueLookup refreshed = repository.findQueuePosition(position.queueToken(), now);
      if (refreshed.status() == AdmissionQueueLookupStatus.EXPIRED) {
        return AdmissionResponse.queueExpired();
      }
      if (refreshed.status() == AdmissionQueueLookupStatus.NOT_QUEUED) {
        return AdmissionResponse.notQueued();
      }
      position = refreshed.position();
    }

    repository.renewQueueEntry(position.queueToken(), now.plus(queueEntryTtl));
    return AdmissionResponse.queued(position.position());
  }

  private AdmissionResponse admitReplacement(long accountId, String clientIp, Instant now) {
    AdmissionReservation reservation =
        newReservation(accountId, clientIp, now, repository.activeSessionId(accountId).orElse(null));
    AdmissionReservationCreation creation = repository.createReplacementReservation(reservation, now);
    return admitted(creation.reservation(), clientIp);
  }

  private AdmissionReservation newReservation(
      long accountId, String clientIp, Instant now, String replacedSessionId) {
    return new AdmissionReservation(
        newToken(),
        accountId,
        nicknameProvider.nicknameFor(accountId),
        clientIp,
        now.plus(reservationTtl),
        reservationTtl,
        replacedSessionId);
  }

  private AdmissionResponse admitted(AdmissionReservation reservation, String clientIp) {
    GameFirewallClient.Decision firewallDecision =
        firewallClient.allowPreAuth(clientIp, preAuthFirewallTtl, "pre-auth");
    if (!firewallDecision.success()) {
      repository.removePendingReservation(reservation);
      return AdmissionResponse.gameServerReadinessFailed();
    }
    return AdmissionResponse.admitted(
        reservation.gameSessionToken(),
        gameServerEndpoint,
        reservation.reservationExpiresAt().toEpochMilli());
  }

  private String newToken() {
    byte[] bytes = new byte[TOKEN_BYTES];
    secureRandom.nextBytes(bytes);
    return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
  }

  private void validateAccountId(long accountId) {
    if (accountId <= 0) {
      throw new IllegalArgumentException("accountId must be positive");
    }
  }
}

enum AdmissionStatus {
  QUEUED("Queued"),
  ADMITTED("Admitted"),
  GAME_SERVER_READINESS_FAILED("GameServerReadinessFailed"),
  QUEUE_EXPIRED("QueueExpired"),
  NOT_QUEUED("NotQueued");

  private final String jsonValue;

  AdmissionStatus(String jsonValue) {
    this.jsonValue = jsonValue;
  }

  @JsonValue
  public String jsonValue() {
    return jsonValue;
  }
}

@JsonInclude(JsonInclude.Include.NON_NULL)
record AdmissionResponse(
    AdmissionStatus status,
    Integer position,
    String queueToken,
    String gameSessionToken,
    GameServerEndpoint gameServerEndpoint,
    Long reservationExpiresAt) {

  static AdmissionResponse queued(int position) {
    if (position < 1) {
      throw new IllegalArgumentException("position must be one-based");
    }
    return new AdmissionResponse(AdmissionStatus.QUEUED, position, null, null, null, null);
  }

  static AdmissionResponse queued(int position, String queueToken) {
    if (position < 1) {
      throw new IllegalArgumentException("position must be one-based");
    }
    return new AdmissionResponse(AdmissionStatus.QUEUED, position, queueToken, null, null, null);
  }

  static AdmissionResponse admitted(
      String gameSessionToken, GameServerEndpoint gameServerEndpoint, long reservationExpiresAt) {
    return new AdmissionResponse(
        AdmissionStatus.ADMITTED,
        null,
        null,
        gameSessionToken,
        gameServerEndpoint,
        reservationExpiresAt);
  }

  static AdmissionResponse queueExpired() {
    return new AdmissionResponse(AdmissionStatus.QUEUE_EXPIRED, null, null, null, null, null);
  }

  static AdmissionResponse gameServerReadinessFailed() {
    return new AdmissionResponse(
        AdmissionStatus.GAME_SERVER_READINESS_FAILED, null, null, null, null, null);
  }

  static AdmissionResponse notQueued() {
    return new AdmissionResponse(AdmissionStatus.NOT_QUEUED, null, null, null, null, null);
  }
}

record GameServerEndpoint(String host, int tcpPort, int rudpPort) {}

record AdmissionReservation(
    String gameSessionToken,
    long accountId,
    String nickname,
    String clientIp,
    Instant reservationExpiresAt,
    Duration ttl,
    String replacedSessionId) {}

record AdmissionReservationCreation(
    AdmissionReservationCreationStatus status, AdmissionReservation reservation) {

  static AdmissionReservationCreation created(AdmissionReservation reservation) {
    return new AdmissionReservationCreation(AdmissionReservationCreationStatus.CREATED, reservation);
  }

  static AdmissionReservationCreation existing(AdmissionReservation reservation) {
    return new AdmissionReservationCreation(
        AdmissionReservationCreationStatus.EXISTING, reservation);
  }

  static AdmissionReservationCreation capacityFull() {
    return new AdmissionReservationCreation(
        AdmissionReservationCreationStatus.CAPACITY_FULL, null);
  }
}

enum AdmissionReservationCreationStatus {
  CREATED,
  EXISTING,
  CAPACITY_FULL
}

record AdmissionQueueEntry(String queueToken, long accountId, Instant expiresAt) {}

record AdmissionQueuePosition(String queueToken, long accountId, int position) {}

record AdmissionQueueLookup(AdmissionQueueLookupStatus status, AdmissionQueuePosition position) {

  static AdmissionQueueLookup queued(AdmissionQueuePosition position) {
    return new AdmissionQueueLookup(AdmissionQueueLookupStatus.QUEUED, position);
  }

  static AdmissionQueueLookup expired() {
    return new AdmissionQueueLookup(AdmissionQueueLookupStatus.EXPIRED, null);
  }

  static AdmissionQueueLookup notQueued() {
    return new AdmissionQueueLookup(AdmissionQueueLookupStatus.NOT_QUEUED, null);
  }

  long accountId() {
    return position.accountId();
  }
}

enum AdmissionQueueLookupStatus {
  QUEUED,
  EXPIRED,
  NOT_QUEUED
}

interface AdmissionNicknameProvider {

  String nicknameFor(long accountId);
}

@Repository
class JdbcAdmissionNicknameProvider implements AdmissionNicknameProvider {

  private final JdbcTemplate jdbcTemplate;

  JdbcAdmissionNicknameProvider(JdbcTemplate jdbcTemplate) {
    this.jdbcTemplate = jdbcTemplate;
  }

  @Override
  public String nicknameFor(long accountId) {
    List<String> nicknames =
        jdbcTemplate.query(
            """
            SELECT nickname
            FROM player_account
            WHERE account_id = ?
            """,
            (resultSet, rowNum) -> resultSet.getString("nickname"),
            accountId);
    if (nicknames.isEmpty() || nicknames.getFirst() == null || nicknames.getFirst().isBlank()) {
      throw new AdmissionNicknameUnavailableException();
    }
    return nicknames.getFirst();
  }
}

final class AdmissionNicknameUnavailableException extends RuntimeException {}
