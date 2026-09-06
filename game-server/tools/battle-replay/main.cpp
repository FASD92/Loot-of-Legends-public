#include <lol/battle_continuity/BattleReplay.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *errorName(lol::battle_continuity::ReplayErrorCode code) noexcept {
  using lol::battle_continuity::ReplayErrorCode;
  switch (code) {
  case ReplayErrorCode::EmptyJournal:
    return "empty_journal";
  case ReplayErrorCode::CodecRejected:
    return "codec_rejected";
  case ReplayErrorCode::UnsupportedRuleset:
    return "unsupported_ruleset";
  case ReplayErrorCode::InvalidStart:
    return "invalid_start";
  case ReplayErrorCode::InitialCheckpointMissing:
    return "initial_checkpoint_missing";
  case ReplayErrorCode::StateDecodeRejected:
    return "state_decode_rejected";
  case ReplayErrorCode::StateHashMismatch:
    return "state_hash_mismatch";
  case ReplayErrorCode::InvalidCommand:
    return "invalid_command";
  case ReplayErrorCode::UnknownCommandKind:
    return "unknown_command_kind";
  case ReplayErrorCode::CommandDecisionMismatch:
    return "command_decision_mismatch";
  case ReplayErrorCode::NoMutationRecord:
    return "no_mutation_record";
  case ReplayErrorCode::StateDivergence:
    return "state_divergence";
  case ReplayErrorCode::CheckpointDivergence:
    return "checkpoint_divergence";
  case ReplayErrorCode::TerminalReceiptMismatch:
    return "terminal_receipt_mismatch";
  case ReplayErrorCode::DuplicateTerminalReceipt:
    return "duplicate_terminal_receipt";
  }
  return "unknown";
}

std::string hex(const lol::battle_continuity::Hash &hash) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : hash) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: lol_battle_replay <journal>\n";
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::cerr << "error=read_failed\n";
    return 2;
  }
  const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{input},
                                        std::istreambuf_iterator<char>{});
  const auto replay =
      lol::battle_continuity::BattleReplayer::replayJournal(bytes);
  if (!replay.ok()) {
    const auto &error = *replay.error;
    std::cout << "FAIL error=" << errorName(error.code)
              << " record_sequence=" << error.recordSequence
              << " logical_tick=" << error.logicalTick << '\n';
    if (error.identity.has_value()) {
      std::cout << "origin_recovery_epoch="
                << error.identity->originRecoveryEpoch
                << " room_id=" << error.identity->roomId.value()
                << " battle_instance_id="
                << error.identity->battleInstanceId.value() << '\n';
    } else {
      std::cout << "origin_recovery_epoch=unavailable room_id=unavailable"
                   " battle_instance_id=unavailable\n";
    }
    if (error.rulesetVersion.has_value()) {
      std::cout << "ruleset_version=" << *error.rulesetVersion << '\n';
    } else {
      std::cout << "ruleset_version=unavailable\n";
    }
    std::cout << "expected_hash="
              << (error.expectedHash.has_value() ? hex(*error.expectedHash)
                                                  : "unavailable")
              << " actual_hash="
              << (error.actualHash.has_value() ? hex(*error.actualHash)
                                                : "unavailable")
              << '\n';
    return 1;
  }
  std::cout << "PASS final_hash=" << hex(*replay.finalStateHash) << '\n';
  return 0;
}
