package com.lol.meta.settlement;

import com.lol.meta.settlement.api.InventoryDeltaRequest;
import com.lol.meta.settlement.api.SettlementRequest;
import com.lol.meta.settlement.api.SettlementResponse;
import com.lol.meta.settlement.api.SettlementStatus;
import com.lol.meta.settlement.repository.InventoryRepository;
import com.lol.meta.settlement.repository.SettlementRecordRepository;
import com.lol.meta.settlement.repository.SettlementRecordRow;
import com.lol.meta.settlement.repository.WalletRepository;
import java.util.Optional;
import org.springframework.dao.DataIntegrityViolationException;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

@Service
public class SettlementService {

  private final InventoryRepository inventoryRepository;
  private final SettlementRecordRepository settlementRecordRepository;
  private final SettlementRequestHasher settlementRequestHasher;
  private final WalletRepository walletRepository;

  public SettlementService(
      InventoryRepository inventoryRepository,
      SettlementRecordRepository settlementRecordRepository,
      SettlementRequestHasher settlementRequestHasher,
      WalletRepository walletRepository) {
    this.inventoryRepository = inventoryRepository;
    this.settlementRecordRepository = settlementRecordRepository;
    this.settlementRequestHasher = settlementRequestHasher;
    this.walletRepository = walletRepository;
  }

  @Transactional
  public SettlementResponse apply(SettlementRequest request) {
    String requestHash = settlementRequestHasher.hash(request);
    Optional<SettlementRecordRow> existing =
        settlementRecordRepository.findBySettlementIdForUpdate(request.settlementId());

    if (existing.isPresent()) {
      if (!existing.get().requestHash().equals(requestHash)) {
        throw new SettlementConflictException(request.settlementId());
      }
      return new SettlementResponse(
          existing.get().settlementId(), SettlementStatus.APPLIED, true);
    }

    settlementRecordRepository.insert(
        new SettlementRecordRow(
            request.settlementId(),
            request.accountId(),
            request.sessionId(),
            request.roomId(),
            SettlementStatus.APPLIED.name(),
            request.goldDelta(),
            requestHash));
    applyAssets(request);

    return new SettlementResponse(request.settlementId(), SettlementStatus.APPLIED, false);
  }

  private void applyAssets(SettlementRequest request) {
    try {
      for (InventoryDeltaRequest delta : request.inventoryDeltas()) {
        inventoryRepository.applyQuantityDelta(
            request.accountId(), delta.itemId(), delta.quantityDelta());
      }

      if (request.goldDelta() == 0L) {
        return;
      }

      int updatedWallets =
          walletRepository.applyGoldDelta(request.accountId(), request.goldDelta());
      if (updatedWallets == 0) {
        throw new SettlementAssetConflictException(
            request.settlementId(), "wallet row does not exist");
      }
    } catch (DataIntegrityViolationException exception) {
      throw new SettlementAssetConflictException(
          request.settlementId(), "asset balance would become negative", exception);
    }
  }
}
