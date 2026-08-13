#include <lol/settlement_storage/JournalRecovery.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#ifndef LOOT_OUTBOX_GOLDEN_DIR
#error "LOOT_OUTBOX_GOLDEN_DIR must name the outbox journal golden directory"
#endif

namespace {

using lol::settlement_storage::JournalRecoveryStatus;
using lol::settlement_storage::recoverJournal;

std::vector<std::uint8_t> readHex(const std::string &name) {
  std::ifstream stream(std::string{LOOT_OUTBOX_GOLDEN_DIR} + "/" + name);
  const std::string text{std::istreambuf_iterator<char>{stream},
                         std::istreambuf_iterator<char>{}};
  std::string compact;
  std::copy_if(text.begin(), text.end(), std::back_inserter(compact),
               [](unsigned char value) { return !std::isspace(value); });

  std::vector<std::uint8_t> bytes;
  bytes.reserve(compact.size() / 2u);
  for (std::size_t offset = 0; offset < compact.size(); offset += 2u) {
    bytes.push_back(static_cast<std::uint8_t>(
        std::stoul(compact.substr(offset, 2u), nullptr, 16)));
  }
  return bytes;
}

bool hasExpectedBatch(const lol::settlement_storage::RecoveredBatch &batch,
                      bool retired) {
  constexpr lol::settlement_storage::JournalBatchId expectedId = {
      0x62, 0x0b, 0x39, 0xc0, 0xdb, 0x27, 0x93, 0x37,
      0x4d, 0x8a, 0x5e, 0xf7, 0xe3, 0x44, 0x15, 0x36};
  return batch.batchId == expectedId && batch.commitSequence == 3u &&
         batch.canonicalIntents.size() == 2u && batch.retired == retired;
}

bool recoversValidCommittedBatch() {
  const auto result = recoverJournal(readHex("outbox-journal-valid.hex"));
  return result.status == JournalRecoveryStatus::Clean &&
         result.durableBytes == 506u && !result.quarantineOffset.has_value() &&
         result.batches.size() == 1u &&
         hasExpectedBatch(result.batches.front(), false);
}

bool quarantinesIncompleteBatchFromItsFirstIntent() {
  const auto result = recoverJournal(readHex("outbox-journal-partial.hex"));
  return result.status == JournalRecoveryStatus::IncompleteTailQuarantined &&
         result.durableBytes == 0u && result.quarantineOffset == 0u &&
         result.batches.empty();
}

bool preservesCommittedPrefixAndDegradesOnCorruptTail() {
  const auto result = recoverJournal(readHex("outbox-journal-corrupt.hex"));
  return result.status == JournalRecoveryStatus::CorruptTailQuarantined &&
         result.durableBytes == 506u && result.quarantineOffset == 506u &&
         result.batches.size() == 1u &&
         hasExpectedBatch(result.batches.front(), false);
}

bool appliesValidRetirement() {
  const auto result = recoverJournal(readHex("outbox-journal-retired.hex"));
  return result.status == JournalRecoveryStatus::Clean &&
         result.durableBytes == 590u && !result.quarantineOffset.has_value() &&
         result.batches.size() == 1u &&
         hasExpectedBatch(result.batches.front(), true);
}

} // namespace

int main() {
  return recoversValidCommittedBatch() &&
                 quarantinesIncompleteBatchFromItsFirstIntent() &&
                 preservesCommittedPrefixAndDegradesOnCorruptTail() &&
                 appliesValidRetirement()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
