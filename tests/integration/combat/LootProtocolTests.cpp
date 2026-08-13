#include <lol/transport/rudp/RudpLootCodec.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lol::transport::rudp;

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
  bytes.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    bytes.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(
        (digit(text[index]) << 4U) | digit(text[index + 1]))));
  }
  return bytes;
}

std::optional<std::string> readGolden() {
  std::ifstream input{LOOT_PROTOCOL_GOLDEN_PATH};
  if (!input) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
}

std::optional<std::vector<std::byte>>
goldenDatagram(std::string_view contract, std::string_view semanticName) {
  const auto message =
      contract.find("\"semanticName\": \"" + std::string{semanticName} + "\"");
  constexpr std::string_view marker = "\"datagramHex\": \"";
  const auto encoded = message == std::string_view::npos
                           ? std::string_view::npos
                           : contract.find(marker, message);
  if (encoded == std::string_view::npos) {
    return std::nullopt;
  }
  const auto first = encoded + marker.size();
  const auto last = contract.find('"', first);
  return last == std::string_view::npos
             ? std::nullopt
             : std::optional{fromHex(contract.substr(first, last - first))};
}

RudpHeader header(RudpFlag flag, std::uint32_t sequence, std::uint32_t ack,
                  std::uint32_t ackBits, std::uint16_t messageId) {
  return {.flag = flag,
          .sessionId = 1,
          .sessionGeneration = 2,
          .transportEpoch = 3,
          .sequence = sequence,
          .ack = ack,
          .ackBits = ackBits,
          .messageId = messageId};
}

bool matchesGoldenVectors() {
  const auto contract = readGolden();
  if (!contract.has_value()) {
    return false;
  }
  const std::array headers{
      header(RudpFlag::Reliable, 9, 8, 15, 32),
      header(RudpFlag::Reliable, 10, 9, 31, 33),
      header(RudpFlag::Reliable, 11, 10, 63, 34),
      header(RudpFlag::Unreliable, 12, 10, 63, 35),
  };
  const std::array<RudpLootMessage, 4> messages{
      RudpClaimLootIntent{
          .commandId = {.high = 0x0102030405060708ULL,
                        .low = 0x1112131415161718ULL},
          .battleInstanceId = 7,
          .dropId = 2,
      },
      RudpClaimLootTerminalResult{
          .commandId = {.high = 0x0102030405060708ULL,
                        .low = 0x1112131415161718ULL},
          .battleInstanceId = 7,
          .dropId = 2,
          .resultCode = RudpClaimLootResultCode::Ok,
      },
      RudpDropSpawned{
          .eventId = {.high = 0x2122232425262728ULL,
                      .low = 0x3132333435363738ULL},
          .battleInstanceId = 7,
          .eventStreamKind = RudpEventStreamKind::LootLifecycle,
          .eventSequence = 1,
          .dropId = 1,
          .itemId = 2,
          .quantity = 1,
          .posXMillimeter = 1000,
          .posYMillimeter = -2000,
          .rulesetVersion = 1,
      },
      RudpDropStateSnapshot{
          .battleInstanceId = 7,
          .snapshotSequence = 9,
          .resolutionState = RudpLootResolutionState::Open,
          .drops =
              {
                  {.dropId = 1,
                   .itemId = 2,
                   .quantity = 1,
                   .posXMillimeter = -1000,
                   .posYMillimeter = 2000,
                   .state = RudpLootDropState::Available,
                   .ownerSessionId = 0},
                  {.dropId = 2,
                   .itemId = 1,
                   .quantity = 1,
                   .posXMillimeter = 1000,
                   .posYMillimeter = -2000,
                   .state = RudpLootDropState::Claimed,
                   .ownerSessionId = 1},
              },
      },
  };
  constexpr std::array names{std::string_view{"ClaimLootIntent"},
                             std::string_view{"ClaimLootTerminalResult"},
                             std::string_view{"DropSpawned"},
                             std::string_view{"DropStateSnapshot"}};
  for (std::size_t index = 0; index < messages.size(); ++index) {
    const auto expected = goldenDatagram(*contract, names[index]);
    const auto encoded = RudpLootCodec::encode(headers[index], messages[index]);
    const auto decoded = expected.has_value() ? RudpLootCodec::decode(*expected)
                                              : DecodedRudpLoot{};
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected || decoded.error != RudpLootCodecError::None ||
        decoded.header != std::optional{headers[index]} ||
        decoded.message != std::optional{messages[index]}) {
      return false;
    }
  }
  return true;
}

bool rejectsClientAuthorityAndMalformedProjection() {
  const RudpClaimLootIntent claim{
      .commandId = {.high = 1, .low = 2},
      .battleInstanceId = 7,
      .dropId = 1,
  };
  auto duplicateDrops = RudpDropStateSnapshot{
      .battleInstanceId = 7,
      .snapshotSequence = 1,
      .resolutionState = RudpLootResolutionState::Open,
      .drops =
          {
              {.dropId = 1,
               .itemId = 1,
               .quantity = 1,
               .posXMillimeter = 0,
               .posYMillimeter = 0,
               .state = RudpLootDropState::Available,
               .ownerSessionId = 0},
              {.dropId = 1,
               .itemId = 2,
               .quantity = 1,
               .posXMillimeter = 0,
               .posYMillimeter = 0,
               .state = RudpLootDropState::Claimed,
               .ownerSessionId = 1},
          },
  };
  auto invalidOwner = duplicateDrops;
  invalidOwner.drops.pop_back();
  invalidOwner.drops.front().ownerSessionId = 1;

  return sizeof(RudpClaimLootIntent) == sizeof(std::uint64_t) * 4 &&
         !RudpLootCodec::encode(header(RudpFlag::Unreliable, 1, 0, 0, 32),
                                RudpLootMessage{claim})
              .has_value() &&
         !RudpLootCodec::encode(header(RudpFlag::Reliable, 1, 0, 0, 35),
                                RudpLootMessage{duplicateDrops})
              .has_value() &&
         !RudpLootCodec::encode(header(RudpFlag::Unreliable, 1, 0, 0, 35),
                                RudpLootMessage{duplicateDrops})
              .has_value() &&
         !RudpLootCodec::encode(header(RudpFlag::Unreliable, 1, 0, 0, 35),
                                RudpLootMessage{invalidOwner})
              .has_value();
}

} // namespace

int main() {
  return matchesGoldenVectors() &&
                 rejectsClientAuthorityAndMalformedProjection()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
