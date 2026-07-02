package com.lol.meta.admission;

import static org.assertj.core.api.Assertions.assertThat;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

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
import org.springframework.data.redis.core.HashOperations;
import org.springframework.data.redis.core.ListOperations;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.data.redis.core.ValueOperations;

class AdmissionServiceTests {
  @Test
  void admitsImmediatelyWhenCapacityAvailable() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(99);
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.enter(123L, Instant.ofEpochMilli(1_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(response.gameSessionToken()).isNotBlank();
    assertThat(response.gameServerEndpoint().tcpPort()).isEqualTo(40000);
    assertThat(response.gameServerEndpoint().rudpPort()).isEqualTo(40000);
    assertThat(response.reservationExpiresAt()).isEqualTo(11_000L);
  }

  @Test
  void admittedEnterCallsFirewallAllowBeforeReturningHandoff() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(99);
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    AdmissionService service =
        AdmissionService.forTest(
            repository, "127.0.0.1", 40000, 40000, firewallClient);

    AdmissionResponse response =
        service.enter(123L, "203.0.113.41", Instant.ofEpochMilli(1_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(response.gameSessionToken()).isNotBlank();
    assertThat(repository.lastReservation().clientIp()).isEqualTo("203.0.113.41");
    assertThat(firewallClient.allowCalls)
        .containsExactly(new FirewallAllowCall("203.0.113.41", Duration.ofSeconds(60), "pre-auth"));
  }

  @Test
  void firewallAllowFailureReturnsReadinessFailureAndCleansPendingReservation() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(99);
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.failure();
    AdmissionService service =
        AdmissionService.forTest(
            repository, "127.0.0.1", 40000, 40000, firewallClient);

    AdmissionResponse response =
        service.enter(123L, "203.0.113.42", Instant.ofEpochMilli(1_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.GAME_SERVER_READINESS_FAILED);
    assertThat(response.gameSessionToken()).isNull();
    assertThat(response.gameServerEndpoint()).isNull();
    assertThat(response.reservationExpiresAt()).isNull();
    assertThat(repository.pendingReservationToken(123L)).isNull();
    assertThat(repository.activeSessionCount()).isEqualTo(99);
  }

  @Test
  void queuedEnterDoesNotCallFirewallAllow() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.enqueueExisting(77L);
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    AdmissionService service =
        AdmissionService.forTest(
            repository, "127.0.0.1", 40000, 40000, firewallClient);

    AdmissionResponse response =
        service.enter(123L, "203.0.113.43", Instant.ofEpochMilli(1_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.QUEUED);
    assertThat(firewallClient.allowCalls).isEmpty();
  }

  @Test
  void queuedStatusPromotionCallsFirewallAllow() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(99);
    repository.enqueueExisting("head-token", 123L, Instant.ofEpochMilli(30_000));
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.success();
    AdmissionService service =
        AdmissionService.forTest(
            repository, "127.0.0.1", 40000, 40000, firewallClient);

    AdmissionResponse response =
        service.status("head-token", "203.0.113.44", Instant.ofEpochMilli(10_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(firewallClient.allowCalls)
        .containsExactly(new FirewallAllowCall("203.0.113.44", Duration.ofSeconds(60), "pre-auth"));
  }

  @Test
  void pendingReservationStillRequiresFirewallReadinessOnRepeatedEnter() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.putPendingReservation(
        new AdmissionReservation(
            "pending-token",
            123L,
            "player123",
            "203.0.113.45",
            Instant.ofEpochMilli(30_000),
            Duration.ofSeconds(10),
            null));
    RecordingGameFirewallClient firewallClient = RecordingGameFirewallClient.failure();
    AdmissionService service =
        AdmissionService.forTest(
            repository, "127.0.0.1", 40000, 40000, firewallClient);

    AdmissionResponse response =
        service.enter(123L, "203.0.113.45", Instant.ofEpochMilli(10_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.GAME_SERVER_READINESS_FAILED);
    assertThat(repository.pendingReservationToken(123L)).isNull();
  }

  @Test
  void queuesWhenCapacityFullAndReportsOneBasedPosition() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.enqueueExisting(77L);
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.enter(123L, Instant.ofEpochMilli(1_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.QUEUED);
    assertThat(response.position()).isEqualTo(2);
    assertThat(response.queueToken()).isNotBlank();
    assertThat(response.gameSessionToken()).isNull();
  }

  @Test
  void queuedStatusDoesNotExposeQueueToken() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.enqueueExisting("queue-token-1", 123L, Instant.ofEpochMilli(30_000));
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.status("queue-token-1", Instant.ofEpochMilli(10_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.QUEUED);
    assertThat(response.position()).isEqualTo(1);
    assertThat(response.queueToken()).isNull();
  }

  @Test
  void storesServerNicknameInReservation() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(99);
    AdmissionService service =
        AdmissionService.forTest(
            repository, "127.0.0.1", 40000, 40000, accountId -> "realPlayer");

    service.enter(123L, Instant.ofEpochMilli(1_000));

    assertThat(repository.lastReservation().nickname()).isEqualTo("realPlayer");
  }

  @Test
  void expiredQueueTokenReturnsQueueExpiredWhileUnknownTokenReturnsNotQueued() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.enqueueExpired("expired-token", 123L);
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse expired = service.status("expired-token", Instant.ofEpochMilli(10_000));
    AdmissionResponse unknown = service.status("unknown-token", Instant.ofEpochMilli(10_000));

    assertThat(expired.status()).isEqualTo(AdmissionStatus.QUEUE_EXPIRED);
    assertThat(unknown.status()).isEqualTo(AdmissionStatus.NOT_QUEUED);
  }

  @Test
  void tokenStatusAtExactExpirationReturnsQueueExpiredWithoutRenewal() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.enqueueExisting("expires-now-token", 123L, Instant.ofEpochMilli(10_000));
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.status("expires-now-token", Instant.ofEpochMilli(10_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.QUEUE_EXPIRED);
    assertThat(repository.renewCount()).isZero();
  }

  @Test
  void accountStatusIgnoresExpiredEntryWithoutRenewal() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.enqueueExisting("head-token", 77L, Instant.ofEpochMilli(30_000));
    repository.enqueueExisting("expires-now-token", 123L, Instant.ofEpochMilli(10_000));
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.status(123L, Instant.ofEpochMilli(10_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.NOT_QUEUED);
    assertThat(repository.renewCount()).isZero();
  }

  @Test
  @SuppressWarnings("unchecked")
  void redisTokenLookupTreatsExactExpirationAsExpired() {
    StringRedisTemplate redisTemplate = mock(StringRedisTemplate.class);
    ListOperations<String, String> listOperations = mock(ListOperations.class);
    HashOperations<String, Object, Object> hashOperations = mock(HashOperations.class);
    ValueOperations<String, String> valueOperations = mock(ValueOperations.class);
    when(redisTemplate.opsForList()).thenReturn(listOperations);
    when(redisTemplate.opsForHash()).thenReturn(hashOperations);
    when(redisTemplate.opsForValue()).thenReturn(valueOperations);
    when(listOperations.range("release0:queue", 0, -1)).thenReturn(List.of("queue-token-1"));
    when(hashOperations.get("release0:queue_entry:queue-token-1", "accountId"))
        .thenReturn("123");
    when(hashOperations.get("release0:queue_entry:queue-token-1", "expiresAt"))
        .thenReturn("10");
    when(valueOperations.get("release0:account_pending_queue:123")).thenReturn("queue-token-1");

    RedisAdmissionQueueRepository repository = new RedisAdmissionQueueRepository(redisTemplate);

    AdmissionQueueLookup lookup =
        repository.findQueuePosition("queue-token-1", Instant.ofEpochMilli(10));

    assertThat(lookup.status()).isEqualTo(AdmissionQueueLookupStatus.EXPIRED);
    verify(listOperations).remove("release0:queue", 0, "queue-token-1");
    verify(redisTemplate).delete("release0:queue_entry:queue-token-1");
    verify(redisTemplate).delete("release0:account_pending_queue:123");
    verify(hashOperations, never()).put("release0:queue_entry:queue-token-1", "expiresAt", "10");
  }

  @Test
  void staleReservationMemberDoesNotBlockNewAdmission() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.markStaleReservationMember();
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.enter(123L, Instant.ofEpochMilli(20_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(repository.activeSessionCount()).isEqualTo(100);
  }

  @Test
  void repeatedEnterWithPendingReservationReturnsSameHandoffWithoutNewCapacity() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(99);
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse first = service.enter(123L, Instant.ofEpochMilli(1_000));
    AdmissionResponse second = service.enter(123L, Instant.ofEpochMilli(2_000));

    assertThat(second.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(second.gameSessionToken()).isEqualTo(first.gameSessionToken());
    assertThat(second.reservationExpiresAt()).isEqualTo(first.reservationExpiresAt());
    assertThat(repository.reservationCreateCount()).isEqualTo(1);
    assertThat(repository.activeSessionCount()).isEqualTo(100);
  }

  @Test
  void repeatedEnterWithPendingQueueReturnsSamePositionAndTokenWithoutDuplicateAppend() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse first = service.enter(123L, Instant.ofEpochMilli(1_000));
    AdmissionResponse second = service.enter(123L, Instant.ofEpochMilli(2_000));

    assertThat(second.status()).isEqualTo(AdmissionStatus.QUEUED);
    assertThat(second.position()).isEqualTo(first.position());
    assertThat(second.queueToken()).isEqualTo(first.queueToken());
    assertThat(repository.queueEntryCount()).isEqualTo(1);
  }

  @Test
  void stalePendingReservationIsCleanedBeforeNewEnter() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.putExpiredPendingReservation(123L, "old-token", Instant.ofEpochMilli(1_000));
    repository.markStaleReservationMember();
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.enter(123L, Instant.ofEpochMilli(20_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(response.gameSessionToken()).isNotEqualTo("old-token");
    assertThat(repository.pendingReservationToken(123L)).isEqualTo(response.gameSessionToken());
  }

  @Test
  void stalePendingQueueIsCleanedBeforeNewEnter() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.putExpiredPendingQueue(123L, "old-queue-token", Instant.ofEpochMilli(1_000));
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.enter(123L, Instant.ofEpochMilli(20_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.QUEUED);
    assertThat(response.position()).isEqualTo(1);
    assertThat(response.queueToken()).isNotEqualTo("old-queue-token");
    assertThat(repository.pendingQueueToken(123L)).isEqualTo(response.queueToken());
  }

  @Test
  void staleQueueEntriesArePurgedBeforeNewEnterPositionIsComputed() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.enqueueExisting("expired-head-token", 77L, Instant.ofEpochMilli(1_000));
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.enter(123L, Instant.ofEpochMilli(20_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.QUEUED);
    assertThat(response.position()).isEqualTo(1);
  }

  @Test
  void cancelClearsPendingQueueIndex() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.enqueueExisting("queue-token-1", 123L, Instant.ofEpochMilli(30_000));
    repository.putPendingQueueIndex(123L, "queue-token-1");
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.cancel("queue-token-1", Instant.ofEpochMilli(10_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.NOT_QUEUED);
    assertThat(repository.pendingQueueToken(123L)).isNull();
  }

  @Test
  void queuedHeadCannotBePromotedTwice() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(98);
    repository.enqueueExisting("head-token", 123L, Instant.ofEpochMilli(30_000));
    repository.keepQueueEntryVisibleAfterRemoval();
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse first = service.status("head-token", Instant.ofEpochMilli(10_000));
    AdmissionResponse second = service.status("head-token", Instant.ofEpochMilli(10_001));

    assertThat(first.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(second.status()).isNotEqualTo(AdmissionStatus.ADMITTED);
    assertThat(repository.reservationCreateCount()).isEqualTo(1);
  }

  @Test
  void activeAccountReplayDoesNotCreateTwoReplacementReservations() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.putActiveSession(123L, "active-session-1");
    repository.hidePendingReservationReads(2);
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse first = service.enter(123L, Instant.ofEpochMilli(1_000));
    AdmissionResponse second = service.enter(123L, Instant.ofEpochMilli(1_001));

    assertThat(second.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(second.gameSessionToken()).isEqualTo(first.gameSessionToken());
    assertThat(repository.reservationCreateCount()).isEqualTo(1);
    assertThat(repository.lastReservation().replacedSessionId()).isEqualTo("active-session-1");
  }

  @Test
  void stalePendingReplacementReservationIsClearedBeforeNewReplacement() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    repository.putActiveSession(123L, "active-session-1");
    repository.putExpiredPendingReservation(123L, "old-token", Instant.ofEpochMilli(1_000));
    AdmissionService service = AdmissionService.forTest(repository, "127.0.0.1", 40000, 40000);

    AdmissionResponse response = service.enter(123L, Instant.ofEpochMilli(20_000));

    assertThat(response.status()).isEqualTo(AdmissionStatus.ADMITTED);
    assertThat(response.gameSessionToken()).isNotEqualTo("old-token");
    assertThat(repository.pendingReservationToken(123L)).isEqualTo(response.gameSessionToken());
    assertThat(repository.reservationCreateCount()).isEqualTo(1);
    assertThat(repository.lastReservation().replacedSessionId()).isEqualTo("active-session-1");
  }

  @Test
  void replacementCreationIsIdempotentWhenPendingReplacementExists() {
    FakeAdmissionQueueRepository repository = new FakeAdmissionQueueRepository(100);
    AdmissionReservation first =
        new AdmissionReservation(
            "replacement-token-1",
            123L,
            "player123",
            "203.0.113.51",
            Instant.ofEpochMilli(11_000),
            Duration.ofSeconds(10),
            "active-session-1");
    AdmissionReservation second =
        new AdmissionReservation(
            "replacement-token-2",
            123L,
            "player123",
            "203.0.113.52",
            Instant.ofEpochMilli(12_000),
            Duration.ofSeconds(10),
            "active-session-1");

    AdmissionReservationCreation firstCreation =
        repository.createReplacementReservation(first, Instant.ofEpochMilli(1_000));
    AdmissionReservationCreation secondCreation =
        repository.createReplacementReservation(second, Instant.ofEpochMilli(1_001));

    assertThat(firstCreation.status()).isEqualTo(AdmissionReservationCreationStatus.CREATED);
    assertThat(secondCreation.status()).isEqualTo(AdmissionReservationCreationStatus.EXISTING);
    assertThat(secondCreation.reservation().gameSessionToken()).isEqualTo("replacement-token-1");
    assertThat(repository.reservationCreateCount()).isEqualTo(1);
  }

  private static final class FakeAdmissionQueueRepository implements AdmissionQueueRepository {

    private final List<AdmissionQueueEntry> queueEntries = new ArrayList<>();
    private final Map<Long, String> activeSessionsByAccount = new HashMap<>();
    private AdmissionReservation lastReservation;
    private int activeSessionCount;
    private int staleReservationMembers;
    private int reservationCreateCount;
    private int renewCount;
    private int hiddenPendingReservationReads;
    private boolean keepQueueEntryVisibleAfterRemoval;
    private final Map<Long, AdmissionReservation> pendingReservationsByAccount = new HashMap<>();
    private final Map<Long, String> pendingQueueTokensByAccount = new HashMap<>();
    private final Set<String> promotedQueueTokens = new HashSet<>();

    FakeAdmissionQueueRepository(int activeSessionCount) {
      this.activeSessionCount = activeSessionCount;
    }

    void enqueueExisting(long accountId) {
      enqueueExisting(
          "existing-token-" + accountId, accountId, Instant.ofEpochMilli(30_000));
    }

    void enqueueExisting(String queueToken, long accountId, Instant expiresAt) {
      queueEntries.add(
          new AdmissionQueueEntry(queueToken, accountId, expiresAt));
    }

    void enqueueExpired(String queueToken, long accountId) {
      queueEntries.add(
          new AdmissionQueueEntry(queueToken, accountId, Instant.ofEpochMilli(1_000)));
    }

    void markStaleReservationMember() {
      staleReservationMembers += 1;
    }

    void putExpiredPendingReservation(long accountId, String token, Instant expiresAt) {
      pendingReservationsByAccount.put(
          accountId,
          new AdmissionReservation(
              token,
              accountId,
              "player" + accountId,
              "",
              expiresAt,
              Duration.ofSeconds(10),
              null));
    }

    void putPendingReservation(AdmissionReservation reservation) {
      pendingReservationsByAccount.put(reservation.accountId(), reservation);
      if (reservation.replacedSessionId() == null) {
        activeSessionCount += 1;
      }
    }

    void putExpiredPendingQueue(long accountId, String queueToken, Instant expiresAt) {
      enqueueExisting(queueToken, accountId, expiresAt);
      pendingQueueTokensByAccount.put(accountId, queueToken);
    }

    void putPendingQueueIndex(long accountId, String queueToken) {
      pendingQueueTokensByAccount.put(accountId, queueToken);
    }

    void keepQueueEntryVisibleAfterRemoval() {
      keepQueueEntryVisibleAfterRemoval = true;
    }

    void putActiveSession(long accountId, String sessionId) {
      activeSessionsByAccount.put(accountId, sessionId);
    }

    void hidePendingReservationReads(int count) {
      hiddenPendingReservationReads = count;
    }

    private void purgeStaleReservations() {
      activeSessionCount -= staleReservationMembers;
      staleReservationMembers = 0;
    }

    private void purgeExpiredQueueEntries(Instant now) {
      for (AdmissionQueueEntry entry : List.copyOf(queueEntries)) {
        if (entry.expiresAt().isBefore(now) || entry.expiresAt().equals(now)) {
          removeQueueEntry(entry.queueToken());
        }
      }
    }

    @Override
    public boolean hasCapacityAvailable(int capacity) {
      return activeSessionCount < capacity;
    }

    @Override
    public boolean hasActiveSession(long accountId) {
      return activeSessionsByAccount.containsKey(accountId);
    }

    @Override
    public Optional<AdmissionReservation> findPendingReservation(long accountId, Instant now) {
      if (hiddenPendingReservationReads > 0) {
        hiddenPendingReservationReads -= 1;
        return Optional.empty();
      }
      AdmissionReservation reservation = pendingReservationsByAccount.get(accountId);
      if (reservation == null) {
        return Optional.empty();
      }
      if (reservation.reservationExpiresAt().isBefore(now)
          || reservation.reservationExpiresAt().equals(now)) {
        pendingReservationsByAccount.remove(accountId);
        return Optional.empty();
      }
      return Optional.of(reservation);
    }

    @Override
    public AdmissionReservationCreation createReservationIfCapacityAvailable(
        AdmissionReservation reservation, int capacity, Instant now) {
      Optional<AdmissionReservation> pending =
          findPendingReservation(reservation.accountId(), now);
      if (pending.isPresent()) {
        return AdmissionReservationCreation.existing(pending.get());
      }
      purgeStaleReservations();
      if (activeSessionCount >= capacity) {
        return AdmissionReservationCreation.capacityFull();
      }
      lastReservation = reservation;
      activeSessionCount += 1;
      reservationCreateCount += 1;
      pendingReservationsByAccount.put(reservation.accountId(), reservation);
      return AdmissionReservationCreation.created(reservation);
    }

    @Override
    public AdmissionReservationCreation createReplacementReservation(
        AdmissionReservation reservation, Instant now) {
      AdmissionReservation pending = pendingReservationsByAccount.get(reservation.accountId());
      if (pending != null && pending.reservationExpiresAt().isAfter(now)) {
        return AdmissionReservationCreation.existing(pending);
      }
      if (pending != null) {
        pendingReservationsByAccount.remove(reservation.accountId());
      }
      lastReservation = reservation;
      reservationCreateCount += 1;
      pendingReservationsByAccount.put(reservation.accountId(), reservation);
      return AdmissionReservationCreation.created(reservation);
    }

    @Override
    public boolean promoteQueuedHeadIfCapacityAvailable(
        String queueToken, AdmissionReservation reservation, int capacity, Instant now) {
      purgeStaleReservations();
      if (promotedQueueTokens.contains(queueToken)
          || queueEntries.isEmpty()
          || !queueEntries.getFirst().queueToken().equals(queueToken)
          || activeSessionCount >= capacity) {
        return false;
      }

      AdmissionQueueEntry entry = queueEntries.getFirst();
      if (entry.accountId() != reservation.accountId()
          || entry.expiresAt().isBefore(now)
          || entry.expiresAt().equals(now)) {
        removeQueueEntry(queueToken);
        return false;
      }

      promotedQueueTokens.add(queueToken);
      lastReservation = reservation;
      activeSessionCount += 1;
      reservationCreateCount += 1;
      pendingReservationsByAccount.put(reservation.accountId(), reservation);
      removeQueueEntry(queueToken);
      return true;
    }

    @Override
    public int activeSessionCount() {
      return activeSessionCount;
    }

    @Override
    public Optional<String> activeSessionId(long accountId) {
      return Optional.ofNullable(activeSessionsByAccount.get(accountId));
    }

    @Override
    public AdmissionQueuePosition enqueue(AdmissionQueueEntry entry, Instant now) {
      purgeExpiredQueueEntries(now);
      Optional<AdmissionQueuePosition> pending =
          findPendingQueuePosition(entry.accountId(), now);
      if (pending.isPresent()) {
        return pending.get();
      }
      queueEntries.add(entry);
      pendingQueueTokensByAccount.put(entry.accountId(), entry.queueToken());
      return new AdmissionQueuePosition(entry.queueToken(), entry.accountId(), queueEntries.size());
    }

    @Override
    public Optional<AdmissionQueuePosition> findQueuePosition(long accountId, Instant now) {
      int visiblePosition = 0;
      for (AdmissionQueueEntry entry : List.copyOf(queueEntries)) {
        if (promotedQueueTokens.contains(entry.queueToken())) {
          continue;
        }
        if (entry.expiresAt().isBefore(now) || entry.expiresAt().equals(now)) {
          removeQueueEntry(entry.queueToken());
          continue;
        }
        visiblePosition += 1;
        if (entry.accountId() == accountId) {
          return Optional.of(
              new AdmissionQueuePosition(entry.queueToken(), accountId, visiblePosition));
        }
      }
      return Optional.empty();
    }

    @Override
    public Optional<AdmissionQueuePosition> findPendingQueuePosition(long accountId, Instant now) {
      String queueToken = pendingQueueTokensByAccount.get(accountId);
      if (queueToken == null) {
        return Optional.empty();
      }
      AdmissionQueueLookup lookup = findQueuePosition(queueToken, now);
      if (lookup.status() == AdmissionQueueLookupStatus.QUEUED
          && lookup.position().accountId() == accountId) {
        return Optional.of(lookup.position());
      }
      pendingQueueTokensByAccount.remove(accountId);
      return Optional.empty();
    }

    @Override
    public AdmissionQueueLookup findQueuePosition(String queueToken, Instant now) {
      int visiblePosition = 0;
      for (AdmissionQueueEntry entry : queueEntries) {
        if (promotedQueueTokens.contains(entry.queueToken())) {
          if (entry.queueToken().equals(queueToken)) {
            return AdmissionQueueLookup.notQueued();
          }
          continue;
        }
        if (entry.expiresAt().isBefore(now) || entry.expiresAt().equals(now)) {
          if (entry.queueToken().equals(queueToken)) {
            return AdmissionQueueLookup.expired();
          }
          continue;
        }
        visiblePosition += 1;
        if (entry.queueToken().equals(queueToken)) {
          return AdmissionQueueLookup.queued(
              new AdmissionQueuePosition(queueToken, entry.accountId(), visiblePosition));
        }
      }
      return AdmissionQueueLookup.notQueued();
    }

    @Override
    public void renewQueueEntry(String queueToken, Instant expiresAt) {
      renewCount += 1;
    }

    @Override
    public boolean cancelQueueEntry(String queueToken) {
      if (keepQueueEntryVisibleAfterRemoval) {
        return true;
      }
      Optional<AdmissionQueueEntry> removed =
          queueEntries.stream()
              .filter(entry -> entry.queueToken().equals(queueToken))
              .findFirst();
      boolean removedAny = queueEntries.removeIf(entry -> entry.queueToken().equals(queueToken));
      removed.ifPresent(
          entry -> pendingQueueTokensByAccount.remove(entry.accountId(), entry.queueToken()));
      return removedAny;
    }

    @Override
    public boolean removeQueueEntry(String queueToken) {
      return cancelQueueEntry(queueToken);
    }

    @Override
    public boolean removePendingReservation(AdmissionReservation reservation) {
      AdmissionReservation pending = pendingReservationsByAccount.get(reservation.accountId());
      if (pending == null
          || !pending.gameSessionToken().equals(reservation.gameSessionToken())) {
        return false;
      }
      pendingReservationsByAccount.remove(reservation.accountId());
      if (pending.replacedSessionId() == null && activeSessionCount > 0) {
        activeSessionCount -= 1;
      }
      return true;
    }

    AdmissionReservation lastReservation() {
      return lastReservation;
    }

    int reservationCreateCount() {
      return reservationCreateCount;
    }

    int renewCount() {
      return renewCount;
    }

    int queueEntryCount() {
      return queueEntries.size();
    }

    String pendingReservationToken(long accountId) {
      AdmissionReservation reservation = pendingReservationsByAccount.get(accountId);
      return reservation == null ? null : reservation.gameSessionToken();
    }

    String pendingQueueToken(long accountId) {
      return pendingQueueTokensByAccount.get(accountId);
    }
  }

  private record FirewallAllowCall(String clientIp, Duration ttl, String reason) {}

  private static final class RecordingGameFirewallClient implements GameFirewallClient {
    private final List<FirewallAllowCall> allowCalls = new ArrayList<>();
    private final boolean allowSuccess;

    private RecordingGameFirewallClient(boolean allowSuccess) {
      this.allowSuccess = allowSuccess;
    }

    static RecordingGameFirewallClient success() {
      return new RecordingGameFirewallClient(true);
    }

    static RecordingGameFirewallClient failure() {
      return new RecordingGameFirewallClient(false);
    }

    @Override
    public Decision allowPreAuth(String clientIp, Duration ttl, String reason) {
      allowCalls.add(new FirewallAllowCall(clientIp, ttl, reason));
      if (allowSuccess) {
        return Decision.ok();
      }
      return Decision.failure("FirewallAgentUnavailable", "firewall agent unavailable");
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
}
