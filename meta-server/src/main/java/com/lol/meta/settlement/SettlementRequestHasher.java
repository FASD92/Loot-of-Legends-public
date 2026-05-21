package com.lol.meta.settlement;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.lol.meta.settlement.api.InventoryDeltaRequest;
import com.lol.meta.settlement.api.SettlementRequest;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import org.springframework.stereotype.Component;

@Component
public final class SettlementRequestHasher {

  private final ObjectMapper objectMapper;

  public SettlementRequestHasher(ObjectMapper objectMapper) {
    this.objectMapper = objectMapper;
  }

  public String hash(SettlementRequest request) {
    byte[] canonicalPayload = canonicalPayload(request);
    byte[] digest = sha256(canonicalPayload);
    return HexFormat.of().formatHex(digest);
  }

  private byte[] canonicalPayload(SettlementRequest request) {
    try {
      return objectMapper.writeValueAsBytes(canonicalJson(request));
    } catch (JsonProcessingException exception) {
      throw new IllegalStateException(
          "Failed to serialize settlement request canonical payload", exception);
    }
  }

  private ObjectNode canonicalJson(SettlementRequest request) {
    ObjectNode root = objectMapper.createObjectNode();
    root.put("settlementId", request.settlementId());
    root.put("sessionId", request.sessionId());
    root.put("accountId", request.accountId());
    root.put("roomId", request.roomId());
    root.put("startedAt", request.startedAt().toString());
    root.put("finishedAt", request.finishedAt().toString());
    root.put("goldDelta", request.goldDelta());

    ArrayNode inventoryDeltas = root.putArray("inventoryDeltas");
    for (InventoryDeltaRequest delta : request.inventoryDeltas()) {
      ObjectNode deltaNode = inventoryDeltas.addObject();
      deltaNode.put("itemId", delta.itemId());
      deltaNode.put("quantityDelta", delta.quantityDelta());
      deltaNode.put("sourceDropId", delta.sourceDropId());
    }

    root.put("reason", request.reason().name());
    return root;
  }

  private byte[] sha256(byte[] canonicalPayload) {
    try {
      return MessageDigest.getInstance("SHA-256").digest(canonicalPayload);
    } catch (NoSuchAlgorithmException exception) {
      throw new IllegalStateException("SHA-256 is not available", exception);
    }
  }
}
