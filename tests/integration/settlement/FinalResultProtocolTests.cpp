#include <lol/transport/tcp/FinalResultProtocol.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using lol::transport::tcp::FinalResult;
using lol::transport::tcp::FinalResultEntry;
using lol::transport::tcp::FinalResultExitStatus;
using lol::transport::tcp::FinalResultOutcome;
using lol::transport::tcp::FinalResultProtocolCodec;

std::vector<std::byte> fromHex(std::string_view text) {
  const auto digit = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    return static_cast<std::uint8_t>(value - 'A' + 10);
  };
  std::vector<std::byte> bytes;
  bytes.reserve(text.size() / 2u);
  for (std::size_t index = 0; index < text.size(); index += 2u) {
    bytes.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(
        (digit(text[index]) << 4u) | digit(text[index + 1u]))));
  }
  return bytes;
}

std::optional<std::string> contract() {
  std::ifstream input{LOOT_FINAL_RESULT_GOLDEN_PATH};
  if (!input) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
}

std::optional<std::vector<std::byte>> golden(std::string_view document,
                                             std::string_view name) {
  const auto semantic =
      document.find("\"semanticName\": \"" + std::string{name} + "\"");
  constexpr std::string_view marker = "\"frameHex\": \"";
  const auto frame = semantic == std::string_view::npos
                         ? std::string_view::npos
                         : document.find(marker, semantic);
  if (frame == std::string_view::npos) {
    return std::nullopt;
  }
  const auto first = frame + marker.size();
  const auto last = document.find('"', first);
  return last == std::string_view::npos
             ? std::nullopt
             : std::optional{fromHex(document.substr(first, last - first))};
}

FinalResultEntry present(std::uint64_t sessionId, std::string nickname,
                         std::uint64_t value, std::uint32_t rank, bool top) {
  return FinalResultEntry{
      .sessionId = sessionId,
      .nickname = std::move(nickname),
      .exitStatus = FinalResultExitStatus::TerminalPresent,
      .finalAssetValue = value,
      .rank = rank,
      .isTop = top,
  };
}

bool exactGoldenVectors() {
  const auto document = contract();
  if (!document.has_value()) {
    return false;
  }
  const std::vector<std::pair<std::string_view, FinalResult>> vectors{
      {"MonsterDefeatedFinalResult",
       FinalResult{
           .roomId = 7,
           .battleInstanceId = 9,
           .outcome = FinalResultOutcome::MonsterDefeated,
           .entries = {present(1, "neo", 300, 1, true),
                       FinalResultEntry{
                           .sessionId = 2,
                           .nickname = "trinity",
                           .exitStatus = FinalResultExitStatus::TerminalExited,
                           .finalAssetValue = 100,
                           .rank = 2,
                           .isTop = false,
                       }},
       }},
      {"CombatTimeoutFinalResult",
       FinalResult{
           .roomId = 7,
           .battleInstanceId = 10,
           .outcome = FinalResultOutcome::CombatTimeout,
           .entries = {present(1, "neo", 0, 0, false),
                       present(2, "trinity", 0, 0, false)},
       }},
      {"CancelledNoActiveParticipantsFinalResult",
       FinalResult{
           .roomId = 7,
           .battleInstanceId = 11,
           .outcome = FinalResultOutcome::CancelledNoActiveParticipants,
           .entries = {FinalResultEntry{
                           .sessionId = 1,
                           .nickname = "neo",
                           .exitStatus = FinalResultExitStatus::TerminalExited,
                           .finalAssetValue = 300,
                           .rank = 0,
                           .isTop = false,
                       },
                       FinalResultEntry{
                           .sessionId = 2,
                           .nickname = "trinity",
                           .exitStatus = FinalResultExitStatus::TerminalExited,
                           .finalAssetValue = 0,
                           .rank = 0,
                           .isTop = false,
                       }},
       }},
  };
  for (const auto &[name, value] : vectors) {
    const auto expected = golden(*document, name);
    const auto actual = FinalResultProtocolCodec::encodeServerFrame(value);
    if (!expected.has_value() || !actual.has_value() || *actual != *expected) {
      return false;
    }
  }
  return true;
}

bool invalidResultsAreRejected() {
  auto invalidRank = FinalResult{
      .roomId = 7,
      .battleInstanceId = 9,
      .outcome = FinalResultOutcome::CombatTimeout,
      .entries = {present(1, "neo", 0, 1, true),
                  present(2, "trinity", 0, 0, false)},
  };
  auto duplicate = FinalResult{
      .roomId = 7,
      .battleInstanceId = 9,
      .outcome = FinalResultOutcome::MonsterDefeated,
      .entries = {present(1, "neo", 300, 1, true),
                  present(1, "trinity", 100, 2, false)},
  };
  auto invalidNickname = FinalResult{
      .roomId = 7,
      .battleInstanceId = 9,
      .outcome = FinalResultOutcome::MonsterDefeated,
      .entries = {present(1, "", 300, 1, true),
                  present(2, "trinity", 100, 2, false)},
  };
  return !FinalResultProtocolCodec::encodeServerFrame(invalidRank)
              .has_value() &&
         !FinalResultProtocolCodec::encodeServerFrame(duplicate).has_value() &&
         !FinalResultProtocolCodec::encodeServerFrame(invalidNickname)
              .has_value();
}

} // namespace

int main() {
  return exactGoldenVectors() && invalidResultsAreRejected() ? EXIT_SUCCESS
                                                             : EXIT_FAILURE;
}
