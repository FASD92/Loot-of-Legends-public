package com.fasd92.lootoflegends.meta.settlement.application;

import com.fasd92.lootoflegends.meta.assets.api.AssetApplyUseCase;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementPendingQuery;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementUseCase;
import com.fasd92.lootoflegends.meta.settlement.domain.SettlementPayload;
import java.util.Base64;
import java.util.HexFormat;
import java.util.Optional;
import java.util.regex.Pattern;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

@Service
public class SettlementApplication implements SettlementUseCase, SettlementPendingQuery {
  private static final Pattern SETTLEMENT_ID = Pattern.compile("[0-9a-f]{32}");
  private static final Pattern PAYLOAD_HASH = Pattern.compile("[0-9a-f]{64}");
  private static final int MIN_ENCODED_PAYLOAD_LENGTH = 132;
  private static final int MAX_ENCODED_PAYLOAD_LENGTH = 1_398_212;

  private final SettlementInbox inbox;
  private final AssetApplyUseCase assets;
  private final SettlementMetrics metrics;

  public SettlementApplication(
      SettlementInbox inbox, AssetApplyUseCase assets, SettlementMetrics metrics) {
    this.inbox = inbox;
    this.assets = assets;
    this.metrics = metrics;
  }

  @Override
  @Transactional
  public Acceptance accept(Submission submission) {
    SettlementPayload canonical = decode(submission);
    try {
      Optional<SettlementInbox.StoredSettlement> existing =
          inbox.findForUpdate(canonical.settlementId());
      if (existing.isPresent()) {
        return retainedAcceptance(submission.settlementId(), canonical, existing.orElseThrow());
      }
      assets.validate(toAssetDelta(canonical));
      inbox.insertIfAbsent(
          canonical.settlementId(),
          canonical.accountId(),
          canonical.payloadHash(),
          canonical.canonicalPayload());
      SettlementInbox.StoredSettlement retained =
          inbox
              .findForUpdate(canonical.settlementId())
              .orElseThrow(
                  () ->
                      new DependencyUnavailable(
                          new IllegalStateException("accepted settlement is missing")));
      return retainedAcceptance(submission.settlementId(), canonical, retained);
    } catch (AssetApplyUseCase.Invalid invalid) {
      throw new Invalid();
    } catch (SettlementInbox.Unavailable | AssetApplyUseCase.DependencyUnavailable unavailable) {
      throw new DependencyUnavailable(unavailable);
    }
  }

  private Acceptance retainedAcceptance(
      String settlementId, SettlementPayload canonical, SettlementInbox.StoredSettlement retained) {
    if (!java.security.MessageDigest.isEqual(retained.payloadHash(), canonical.payloadHash())) {
      metrics.recordConflict();
      return new Conflict();
    }
    return new Accepted(new Settlement(settlementId, retained.status()));
  }

  private static AssetApplyUseCase.AssetDelta toAssetDelta(SettlementPayload payload) {
    return new AssetApplyUseCase.AssetDelta(
        payload.accountId(),
        payload.catalogVersion(),
        payload.itemDeltas().stream()
            .map(item -> new AssetApplyUseCase.ItemDelta(item.itemId(), item.quantity()))
            .toList(),
        payload.finalAssetValue());
  }

  @Override
  @Transactional(readOnly = true)
  public Optional<Settlement> find(String settlementId) {
    byte[] id = decodeId(settlementId);
    try {
      return inbox.find(id).map(stored -> new Settlement(settlementId, stored.status()));
    } catch (SettlementInbox.Unavailable unavailable) {
      throw new DependencyUnavailable(unavailable);
    }
  }

  @Override
  @Transactional(readOnly = true)
  public long countPending(AccountId accountId) {
    try {
      return inbox.countPending(accountId);
    } catch (SettlementInbox.Unavailable unavailable) {
      throw new SettlementPendingQuery.Unavailable(unavailable);
    }
  }

  private static SettlementPayload decode(Submission submission) {
    if (submission == null
        || submission.payloadHash() == null
        || !PAYLOAD_HASH.matcher(submission.payloadHash()).matches()
        || submission.canonicalPayload() == null
        || submission.canonicalPayload().length() < MIN_ENCODED_PAYLOAD_LENGTH
        || submission.canonicalPayload().length() > MAX_ENCODED_PAYLOAD_LENGTH) {
      throw new Invalid();
    }
    byte[] settlementId = decodeId(submission.settlementId());
    byte[] payloadHash;
    byte[] payload;
    try {
      payloadHash = HexFormat.of().parseHex(submission.payloadHash());
      payload = Base64.getDecoder().decode(submission.canonicalPayload());
    } catch (IllegalArgumentException invalidEncoding) {
      throw new Invalid();
    }
    if (!Base64.getEncoder().encodeToString(payload).equals(submission.canonicalPayload())) {
      throw new Invalid();
    }
    try {
      return SettlementPayload.parse(settlementId, payloadHash, payload);
    } catch (SettlementPayload.Invalid invalidPayload) {
      throw new Invalid();
    }
  }

  private static byte[] decodeId(String settlementId) {
    if (settlementId == null || !SETTLEMENT_ID.matcher(settlementId).matches()) {
      throw new Invalid();
    }
    return HexFormat.of().parseHex(settlementId);
  }
}
