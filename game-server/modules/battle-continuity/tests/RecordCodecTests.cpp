#include <lol/battle_continuity/RecordCodec.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace {

using namespace lol::battle_continuity;

constexpr std::uint64_t kRoomId = (7ULL << 32U) | 11U;
constexpr std::uint64_t kBattleId = 31U;
constexpr std::uint32_t kOriginEpoch = 7U;
constexpr std::uint32_t kWriterEpoch = 8U;

RecordHeader header(RecordType type, std::uint64_t sequence,
                    std::uint64_t tick = 0) {
  return RecordHeader{.recordType = type,
                      .recordSequence = sequence,
                      .originRecoveryEpoch = kOriginEpoch,
                      .writerRecoveryEpoch = kWriterEpoch,
                      .roomId = lol::shared::RoomId{kRoomId},
                      .battleInstanceId =
                          lol::shared::BattleInstanceId{kBattleId},
                      .logicalTick = tick,
                      .battleElapsedNanos = tick * kTickNanos};
}

Record battleStart() {
  const auto initialStateHash = canonicalStateHash(Bytes{0x10U, 0x20U, 0x30U});
  if (!initialStateHash.has_value()) {
    std::abort();
  }
  return Record{
      .header = header(RecordType::BattleStart, 1),
      .payload = BattleStartPayload{
          .rulesetVersion = kSupportedBattleRulesetVersion,
          .battleSeed = 0x0123456789abcdefULL,
          .tickHertz = kTickHertz,
          .initialStateHash = *initialStateHash,
          .participants = {
              {.participantSlot = 1, .sessionId = lol::shared::SessionId{100}},
              {.participantSlot = 2,
               .sessionId = lol::shared::SessionId{200}}}}};
}

Record commandDecision(std::uint64_t sequence, std::uint64_t tick,
                       std::uint64_t commandLow = 42) {
  return Record{.header = header(RecordType::CommandDecision, sequence, tick),
                .payload = CommandDecisionPayload{
                    .participantSlot = 1,
                    .commandKind = 2,
                    .commandId = CommandId{.high = 3, .low = commandLow},
                    .decisionCode = 0,
                    .commandPayload = {0x01U, 0x00U, 0x7fU},
                    .outcomePayload = {0x02U},
                    .roomRecoveryStateBytes = {0x03U, 0x04U},
                    .postDecisionStateHash = Hash{0x22U}}};
}

Record checkpoint(std::uint64_t sequence, std::uint64_t tick) {
  const Bytes stateBytes{0x10U, 0x20U, 0x30U};
  const auto stateHash = canonicalStateHash(stateBytes);
  if (!stateHash.has_value()) {
    std::abort();
  }
  return Record{.header = header(RecordType::Checkpoint, sequence, tick),
                .payload = CheckpointPayload{
                    .checkpointSchemaVersion = kCheckpointSchemaVersion,
                                             .canonicalStateBytes = stateBytes,
                                             .stateHash = *stateHash}};
}

Record terminal(std::uint64_t sequence, std::uint64_t tick) {
  return Record{
      .header = header(RecordType::TerminalReceipt, sequence, tick),
      .payload = TerminalReceiptPayload{
          .terminalReason = 1,
          .finalStateHash = Hash{0x33U},
          .canonicalResultPayload = {0x04U, 0x05U},
          .resultCommittedUnixEpochMilliseconds = 1'700'000'000'000ULL,
          .resultCommittedBattleElapsedNanos = tick * kTickNanos,
          .settlementBatchId = SettlementBatchId{0x44U},
          .settlementIntentCount = 1,
          .settlements = {{.participantSlot = 1,
                           .settlementId = SettlementId{0x55U},
                           .settlementPayloadHash = Hash{0x66U}}}}};
}

Record initialCheckpoint(std::uint64_t sequence = 2) {
  const Bytes stateBytes{0x10U, 0x20U, 0x30U};
  const auto stateHash = canonicalStateHash(stateBytes);
  if (!stateHash.has_value()) {
    std::abort();
  }
  return Record{.header = header(RecordType::Checkpoint, sequence, 0),
                .payload = CheckpointPayload{
                    .checkpointSchemaVersion = kCheckpointSchemaVersion,
                                             .canonicalStateBytes = stateBytes,
                                             .stateHash = *stateHash}};
}

Record tickCommit(std::uint64_t sequence, std::uint64_t tick,
                  std::uint64_t first, std::uint64_t last, std::uint32_t count,
                  Hash committedStateHash = Hash{0x77U}) {
  return Record{
      .header = header(RecordType::TickCommit, sequence, tick),
      .payload = TickCommitPayload{.firstRecordSequence = first,
                                   .lastDataRecordSequence = last,
                                   .recordCount = count,
                                   .committedStateHash = committedStateHash}};
}

bool sameBytesAndHash() {
  const auto encoded = encodeRecord(battleStart());
  if (!encoded.ok()) {
    return false;
  }
  const auto decoded = decodeRecord(encoded.bytes);
  const auto hash = canonicalStateHash(encoded.bytes);
  return decoded.ok() && encodeRecord(*decoded.record).bytes == encoded.bytes &&
         hash.has_value() && hash == canonicalStateHash(encoded.bytes);
}

bool canonicalHashAndCrcUseKnownVectors() {
  const Hash emptySha256{0xe3U, 0xb0U, 0xc4U, 0x42U, 0x98U, 0xfcU, 0x1cU,
                         0x14U, 0x9aU, 0xfbU, 0xf4U, 0xc8U, 0x99U, 0x6fU,
                         0xb9U, 0x24U, 0x27U, 0xaeU, 0x41U, 0xe4U, 0x64U,
                         0x9bU, 0x93U, 0x4cU, 0xa4U, 0x95U, 0x99U, 0x1bU,
                         0x78U, 0x52U, 0xb8U, 0x55U};
  const Bytes crcVector{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  const auto emptyHash = canonicalStateHash(Bytes{});
  return emptyHash.has_value() && *emptyHash == emptySha256 &&
         crc32(crcVector) == 0xcbf43926U;
}

bool envelopeIsBigEndian() {
  const auto encoded = encodeRecord(commandDecision(2, 0x01020304ULL));
  if (!encoded.ok() || encoded.bytes.size() < kRecordEnvelopeBytes) {
    return false;
  }
  return encoded.bytes[0] == 0x4cU && encoded.bytes[1] == 0x42U &&
         encoded.bytes[2] == 0x43U && encoded.bytes[3] == 0x31U &&
         encoded.bytes[12] == 0x00U && encoded.bytes[19] == 0x02U &&
         encoded.bytes[48] == 0x01U && encoded.bytes[51] == 0x04U;
}

bool recordAndCheckpointSchemasMatchV1Contract() {
  const auto command = encodeRecord(commandDecision(2, 1));
  const auto saved = encodeRecord(checkpoint(2, 1));
  return command.ok() && saved.ok() && command.bytes.size() > 5U &&
         saved.bytes.size() > 65U && command.bytes[4] == 0x00U &&
         command.bytes[5] == 0x01U && saved.bytes[4] == 0x00U &&
         saved.bytes[5] == 0x01U && saved.bytes[64] == 0x00U &&
         saved.bytes[65] == 0x01U;
}

bool orderTamperReportsFirstDivergence() {
  const std::vector<HashObservation> expected{
      {.recordSequence = 2, .logicalTick = 1, .stateHash = Hash{0x10U}},
      {.recordSequence = 3, .logicalTick = 2, .stateHash = Hash{0x20U}}};
  const std::vector<HashObservation> actual{
      {.recordSequence = 2, .logicalTick = 1, .stateHash = Hash{0x10U}},
      {.recordSequence = 4, .logicalTick = 2, .stateHash = Hash{0x99U}}};
  const auto divergence = firstDivergence(expected, actual);
  return divergence.has_value() && divergence->recordSequence == 4 &&
         divergence->logicalTick == 2;
}

bool truncatedAndChecksumRejected() {
  const auto encoded = encodeRecord(commandDecision(2, 1));
  if (!encoded.ok() || encoded.bytes.size() < 2) {
    return false;
  }
  const auto truncated =
      decodeRecord(std::span<const std::uint8_t>{encoded.bytes}.first(
          encoded.bytes.size() - 1));
  auto corruptedBytes = encoded.bytes;
  corruptedBytes.back() ^= 0x01U;
  const auto corrupted = decodeRecord(corruptedBytes);
  return !truncated.ok() &&
         truncated.error->code == CodecErrorCode::Truncated &&
         !corrupted.ok() &&
         corrupted.error->code == CodecErrorCode::ChecksumMismatch;
}

bool unsupportedRulesetRejected() {
  auto encoded = encodeRecord(battleStart());
  if (!encoded.ok()) {
    return false;
  }
  // BattleStart rulesetVersion is the first payload u32 at envelope offset 64.
  encoded.bytes[67] = 2U;
  const std::uint32_t checksum =
      crc32(std::span<const std::uint8_t>{encoded.bytes}.first(
          encoded.bytes.size() - sizeof(std::uint32_t)));
  encoded.bytes[encoded.bytes.size() - 4U] =
      static_cast<std::uint8_t>(checksum >> 24U);
  encoded.bytes[encoded.bytes.size() - 3U] =
      static_cast<std::uint8_t>(checksum >> 16U);
  encoded.bytes[encoded.bytes.size() - 2U] =
      static_cast<std::uint8_t>(checksum >> 8U);
  encoded.bytes[encoded.bytes.size() - 1U] =
      static_cast<std::uint8_t>(checksum);
  const auto decoded = decodeRecord(encoded.bytes);
  return !decoded.ok() &&
         decoded.error->code == CodecErrorCode::UnsupportedRulesetVersion;
}

bool unsupportedRecordSchemaRejected() {
  auto encoded = encodeRecord(battleStart());
  if (!encoded.ok() || encoded.bytes.size() < 6U) {
    return false;
  }
  encoded.bytes[4] = 0U;
  encoded.bytes[5] = 2U;
  const auto decoded = decodeRecord(encoded.bytes);
  return !decoded.ok() && decoded.error.has_value() &&
         decoded.error->code == CodecErrorCode::UnsupportedSchemaVersion;
}

bool journalRoundTripsAllRecordTypes() {
  const auto checkpointHash =
      std::get<CheckpointPayload>(initialCheckpoint().payload).stateHash;
  const auto first = encodeRecord(battleStart());
  const auto second = encodeRecord(initialCheckpoint());
  const auto third = encodeRecord(tickCommit(3, 0, 1, 2, 2, checkpointHash));
  const auto fourth = encodeRecord(commandDecision(4, 1));
  const auto fifth = encodeRecord(checkpoint(5, 1));
  const auto sixth = encodeRecord(tickCommit(6, 1, 4, 5, 2, checkpointHash));
  const auto seventh = encodeRecord(terminal(7, 2));
  const auto eighth = encodeRecord(checkpoint(8, 2));
  const auto ninth = encodeRecord(tickCommit(9, 2, 7, 8, 2, checkpointHash));
  if (!first.ok() || !second.ok() || !third.ok() || !fourth.ok() ||
      !fifth.ok() || !sixth.ok() || !seventh.ok() || !eighth.ok() ||
      !ninth.ok()) {
    return false;
  }
  Bytes bytes = first.bytes;
  bytes.insert(bytes.end(), second.bytes.begin(), second.bytes.end());
  bytes.insert(bytes.end(), third.bytes.begin(), third.bytes.end());
  bytes.insert(bytes.end(), fourth.bytes.begin(), fourth.bytes.end());
  bytes.insert(bytes.end(), fifth.bytes.begin(), fifth.bytes.end());
  bytes.insert(bytes.end(), sixth.bytes.begin(), sixth.bytes.end());
  bytes.insert(bytes.end(), seventh.bytes.begin(), seventh.bytes.end());
  bytes.insert(bytes.end(), eighth.bytes.begin(), eighth.bytes.end());
  bytes.insert(bytes.end(), ninth.bytes.begin(), ninth.bytes.end());
  const auto journal = decodeJournal(bytes);
  return journal.ok() && journal.records.size() == 9 &&
         journal.committedBytes == bytes.size() &&
         std::get_if<CheckpointPayload>(&journal.records[4].payload) !=
             nullptr &&
         std::get_if<TerminalReceiptPayload>(&journal.records[6].payload) !=
             nullptr &&
         std::get_if<TickCommitPayload>(&journal.records[8].payload) != nullptr;
}

Bytes encodeJournal(const std::vector<Record> &records) {
  Bytes bytes;
  for (const auto &record : records) {
    const auto encoded = encodeRecord(record);
    if (!encoded.ok()) {
      return {};
    }
    bytes.insert(bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
  }
  return bytes;
}

std::vector<Record> validJournal() {
  const auto initialHash =
      std::get<CheckpointPayload>(initialCheckpoint().payload).stateHash;
  const auto stateHash =
      std::get<CommandDecisionPayload>(commandDecision(4, 1).payload)
          .postDecisionStateHash;
  return {battleStart(), initialCheckpoint(),
          tickCommit(3, 0, 1, 2, 2, initialHash), commandDecision(4, 1),
          tickCommit(5, 1, 4, 4, 1, stateHash)};
}

bool journalRejectsCommitPayloadMismatch() {
  auto records = validJournal();
  auto &commit = std::get<TickCommitPayload>(records.back().payload);
  commit.recordCount = 2;
  const auto decoded = decodeJournal(encodeJournal(records));
  if (decoded.ok() ||
      decoded.error->code != CodecErrorCode::BatchInvariantViolation) {
    return false;
  }

  records = validJournal();
  auto &range = std::get<TickCommitPayload>(records.back().payload);
  range.firstRecordSequence = 2;
  const auto rangeDecoded = decodeJournal(encodeJournal(records));
  if (rangeDecoded.ok() ||
      rangeDecoded.error->code != CodecErrorCode::BatchInvariantViolation) {
    return false;
  }

  records = validJournal();
  auto &hash = std::get<TickCommitPayload>(records.back().payload);
  hash.committedStateHash = Hash{0x99U};
  const auto hashDecoded = decodeJournal(encodeJournal(records));
  return !hashDecoded.ok() &&
         hashDecoded.error->code == CodecErrorCode::BatchInvariantViolation;
}

bool secondBattleStartRejected() {
  auto firstStart = battleStart();
  const auto initialHash =
      std::get<BattleStartPayload>(firstStart.payload).initialStateHash;
  auto records = std::vector<Record>{firstStart, initialCheckpoint(),
                                     tickCommit(3, 0, 1, 2, 2, initialHash)};
  auto secondStart = battleStart();
  secondStart.header.recordSequence = 4;
  records.push_back(secondStart);
  return !decodeJournal(encodeJournal(records)).ok();
}

Record withWriter(Record record, std::uint32_t writerEpoch) {
  record.header.writerRecoveryEpoch = writerEpoch;
  return record;
}

bool writerEpochRegressionRejected() {
  auto records = validJournal();
  const auto secondBatchCommand = withWriter(commandDecision(6, 2), 7);
  const auto commandHash =
      std::get<CommandDecisionPayload>(secondBatchCommand.payload)
          .postDecisionStateHash;
  records.push_back(secondBatchCommand);
  records.push_back(withWriter(tickCommit(7, 2, 6, 6, 1, commandHash), 7));
  const auto decoded = decodeJournal(encodeJournal(records));
  return !decoded.ok() &&
         decoded.error->code == CodecErrorCode::WriterEpochRegression;
}

bool writerEpochChangeWithinBatchRejected() {
  auto records = validJournal();
  const auto secondBatchCommand = withWriter(commandDecision(6, 2), 9);
  const auto commandHash =
      std::get<CommandDecisionPayload>(secondBatchCommand.payload)
          .postDecisionStateHash;
  records.push_back(secondBatchCommand);
  records.push_back(withWriter(tickCommit(7, 2, 6, 6, 1, commandHash), 10));
  const auto decoded = decodeJournal(encodeJournal(records));
  return !decoded.ok() &&
         decoded.error->code == CodecErrorCode::WriterEpochPerBatch;
}

bool writerEpochIncreaseAtBatchBoundaryAccepted() {
  auto records = validJournal();
  const auto secondBatchCommand = withWriter(commandDecision(6, 2), 9);
  const auto commandHash =
      std::get<CommandDecisionPayload>(secondBatchCommand.payload)
          .postDecisionStateHash;
  records.push_back(secondBatchCommand);
  records.push_back(withWriter(tickCommit(7, 2, 6, 6, 1, commandHash), 9));
  const auto decoded = decodeJournal(encodeJournal(records));
  return decoded.ok() && decoded.records.size() == 7;
}

bool uncommittedJournalEndRejected() {
  const auto records = std::vector<Record>{battleStart(), initialCheckpoint()};
  const auto bytes = encodeJournal(records);
  const auto decoded = decodeJournal(bytes);
  return !decoded.ok() &&
         decoded.error->code == CodecErrorCode::JournalNotCommitted &&
         decoded.records.empty() && decoded.committedBytes == 0U;
}

bool invalidJournalSequenceRejected() {
  const auto first = encodeRecord(battleStart());
  const auto second = encodeRecord(commandDecision(3, 1));
  if (!first.ok() || !second.ok()) {
    return false;
  }
  Bytes bytes = first.bytes;
  bytes.insert(bytes.end(), second.bytes.begin(), second.bytes.end());
  const auto journal = decodeJournal(bytes);
  return !journal.ok() &&
         journal.error->code == CodecErrorCode::InvalidSequence;
}

bool terminalFieldsRoundTrip() {
  const auto encoded = encodeRecord(terminal(9, 8));
  if (!encoded.ok()) {
    return false;
  }
  const auto decoded = decodeRecord(encoded.bytes);
  if (!decoded.ok()) {
    return false;
  }
  const auto *payload =
      std::get_if<TerminalReceiptPayload>(&decoded.record->payload);
  return payload != nullptr &&
         payload->resultCommittedUnixEpochMilliseconds ==
             1'700'000'000'000ULL &&
         payload->resultCommittedBattleElapsedNanos == 8 * kTickNanos &&
         payload->settlementIntentCount == 1 &&
         payload->settlements.size() == 1 &&
         payload->settlements.front().participantSlot == 1U;
}

bool firstBatchRequiresInitialCheckpoint() {
  const auto start = battleStart();
  auto missing = std::vector<Record>{start, commandDecision(2, 0),
                                     tickCommit(3, 0, 1, 2, 2, Hash{0x22U})};
  const auto missingDecoded = decodeJournal(encodeJournal(missing));
  if (missingDecoded.ok() || missingDecoded.records.size() != 0U ||
      missingDecoded.committedBytes != 0U ||
      missingDecoded.error->code != CodecErrorCode::BatchInvariantViolation) {
    return false;
  }

  auto mismatchedStart = start;
  std::get<BattleStartPayload>(mismatchedStart.payload).initialStateHash =
      Hash{0xaau};
  auto mismatched = std::vector<Record>{
      mismatchedStart, initialCheckpoint(),
      tickCommit(
          3, 0, 1, 2, 2,
          std::get<CheckpointPayload>(initialCheckpoint().payload).stateHash)};
  const auto mismatchedDecoded = decodeJournal(encodeJournal(mismatched));
  return !mismatchedDecoded.ok() && mismatchedDecoded.records.empty() &&
         mismatchedDecoded.committedBytes == 0U &&
         mismatchedDecoded.error->code ==
             CodecErrorCode::BatchInvariantViolation;
}

bool journalReturnsOnlyCommittedPrefixAndOffset() {
  const auto committed = encodeJournal(validJournal());
  if (committed.empty()) {
    return false;
  }

  auto uncommittedBytes = committed;
  const auto tail = encodeRecord(commandDecision(6, 2));
  if (!tail.ok()) {
    return false;
  }
  uncommittedBytes.insert(uncommittedBytes.end(), tail.bytes.begin(),
                          tail.bytes.end());
  const auto uncommitted = decodeJournal(uncommittedBytes);
  if (uncommitted.ok() || uncommitted.records.size() != validJournal().size() ||
      uncommitted.committedBytes != committed.size() ||
      uncommitted.error->code != CodecErrorCode::JournalNotCommitted) {
    return false;
  }

  auto tornBytes = uncommittedBytes;
  tornBytes.pop_back();
  const auto torn = decodeJournal(tornBytes);
  if (torn.ok() || torn.records.size() != validJournal().size() ||
      torn.committedBytes != committed.size() ||
      torn.error->code != CodecErrorCode::Truncated) {
    return false;
  }

  auto corruptBytes = committed;
  corruptBytes.insert(corruptBytes.end(), tail.bytes.begin(), tail.bytes.end());
  corruptBytes.back() ^= 0x01U;
  const auto corrupt = decodeJournal(corruptBytes);
  return !corrupt.ok() && corrupt.records.size() == validJournal().size() &&
         corrupt.committedBytes == committed.size() &&
         corrupt.error->code == CodecErrorCode::ChecksumMismatch;
}

bool settlementReceiptValidation() {
  auto zeroSlot = terminal(4, 2);
  std::get<TerminalReceiptPayload>(zeroSlot.payload)
      .settlements[0]
      .participantSlot = 0U;
  const auto zeroSlotEncoded = encodeRecord(zeroSlot);
  if (zeroSlotEncoded.ok() ||
      zeroSlotEncoded.error->code != CodecErrorCode::InvalidPayload) {
    return false;
  }

  auto duplicate = terminal(4, 2);
  auto &duplicatePayload = std::get<TerminalReceiptPayload>(duplicate.payload);
  duplicatePayload.settlementIntentCount = 2U;
  duplicatePayload.settlements.push_back(duplicatePayload.settlements.front());
  duplicatePayload.settlements.back().participantSlot = 2U;
  const auto duplicateEncoded = encodeRecord(duplicate);
  if (duplicateEncoded.ok() ||
      duplicateEncoded.error->code != CodecErrorCode::InvalidPayload) {
    return false;
  }

  auto outOfOrder = terminal(4, 2);
  auto &outOfOrderPayload =
      std::get<TerminalReceiptPayload>(outOfOrder.payload);
  outOfOrderPayload.settlementIntentCount = 2U;
  auto second = outOfOrderPayload.settlements.front();
  second.participantSlot = 1U;
  outOfOrderPayload.settlements.front().participantSlot = 2U;
  outOfOrderPayload.settlements.push_back(second);
  const auto outOfOrderEncoded = encodeRecord(outOfOrder);
  return !outOfOrderEncoded.ok() &&
         outOfOrderEncoded.error->code == CodecErrorCode::InvalidPayload;
}

bool mixedLogicalTickBatchRejected() {
  const auto initialHash =
      std::get<CheckpointPayload>(initialCheckpoint().payload).stateHash;
  const auto bytes = encodeJournal({battleStart(), initialCheckpoint(),
                                    tickCommit(3, 0, 1, 2, 2, initialHash),
                                    commandDecision(4, 1), checkpoint(5, 2),
                                    tickCommit(6, 2, 4, 5, 2, Hash{0x22U})});
  const auto decoded = decodeJournal(bytes);
  return !decoded.ok() && decoded.error.has_value() &&
         decoded.error->code == CodecErrorCode::BatchInvariantViolation;
}

bool terminalWithoutCheckpointRejected() {
  const auto initialHash =
      std::get<CheckpointPayload>(initialCheckpoint().payload).stateHash;
  const auto terminalHash =
      std::get<TerminalReceiptPayload>(terminal(4, 2).payload).finalStateHash;
  const auto decoded = decodeJournal(
      encodeJournal({battleStart(), initialCheckpoint(),
                     tickCommit(3, 0, 1, 2, 2, initialHash), terminal(4, 2),
                     tickCommit(5, 2, 4, 4, 1, terminalHash)}));
  return !decoded.ok() && decoded.error.has_value() &&
         decoded.error->code == CodecErrorCode::BatchInvariantViolation;
}

bool run(const char *name, bool (*test)()) {
  const bool passed = test();
  if (!passed) {
    std::cerr << "FAIL: " << name << '\n';
  }
  return passed;
}

} // namespace

int main() {
  return run("same bytes and hash", sameBytesAndHash) &&
                 run("known hash and crc",
                     canonicalHashAndCrcUseKnownVectors) &&
                 run("big endian envelope", envelopeIsBigEndian) &&
                 run("v1 record and checkpoint schemas",
                     recordAndCheckpointSchemasMatchV1Contract) &&
                 run("first divergence", orderTamperReportsFirstDivergence) &&
                 run("truncated and checksum", truncatedAndChecksumRejected) &&
                 run("unsupported ruleset", unsupportedRulesetRejected) &&
                 run("unsupported record schema",
                     unsupportedRecordSchemaRejected) &&
                 run("journal all record types",
                     journalRoundTripsAllRecordTypes) &&
                 run("journal commit invariants",
                     journalRejectsCommitPayloadMismatch) &&
                 run("second battle start", secondBattleStartRejected) &&
                 run("writer epoch regression",
                     writerEpochRegressionRejected) &&
                 run("writer epoch per batch",
                     writerEpochChangeWithinBatchRejected) &&
                 run("writer epoch boundary",
                     writerEpochIncreaseAtBatchBoundaryAccepted) &&
                 run("uncommitted journal end",
                     uncommittedJournalEndRejected) &&
                 run("journal sequence", invalidJournalSequenceRejected) &&
                 run("terminal fields", terminalFieldsRoundTrip) &&
                 run("first batch initial checkpoint",
                     firstBatchRequiresInitialCheckpoint) &&
                 run("journal committed prefix",
                     journalReturnsOnlyCommittedPrefixAndOffset) &&
                 run("settlement receipt validation",
                     settlementReceiptValidation) &&
                 run("mixed logical tick batch",
                     mixedLogicalTickBatchRejected) &&
                 run("terminal checkpoint", terminalWithoutCheckpointRejected)
             ? 0
             : 1;
}
