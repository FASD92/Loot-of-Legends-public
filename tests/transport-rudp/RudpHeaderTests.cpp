#include <lol/transport/rudp/RudpHeader.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using lol::transport::rudp::DecodedRudpDatagram;
using lol::transport::rudp::isSequenceNewer;
using lol::transport::rudp::RudpFlag;
using lol::transport::rudp::RudpHeader;
using lol::transport::rudp::RudpHeaderCodec;
using lol::transport::rudp::RudpHeaderError;

constexpr std::size_t kHeaderBytes = 48;

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

std::optional<std::string> readText(const char *path) {
  std::ifstream input{path};
  if (!input) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
}

std::optional<std::vector<std::byte>>
goldenDatagram(std::string_view contract, std::string_view semanticName) {
  const std::string marker =
      "\"semanticName\": \"" + std::string{semanticName} + "\"";
  const auto message = contract.find(marker);
  if (message == std::string_view::npos) {
    return std::nullopt;
  }
  constexpr std::string_view datagramMarker = "\"datagramHex\": \"";
  const auto datagram = contract.find(datagramMarker, message);
  if (datagram == std::string_view::npos) {
    return std::nullopt;
  }
  const auto first = datagram + datagramMarker.size();
  const auto last = contract.find('"', first);
  return last == std::string_view::npos
             ? std::nullopt
             : std::optional{fromHex(contract.substr(first, last - first))};
}

struct GoldenCase final {
  std::string_view semanticName;
  const std::string *contract;
  RudpHeader header;
};

bool frozenGoldenVectorsRoundTrip() {
  const auto bind = readText(LOOT_RUDP_BIND_GOLDEN_PATH);
  const auto movement = readText(LOOT_MOVEMENT_GOLDEN_PATH);
  if (!bind.has_value() || !movement.has_value()) {
    return false;
  }

  const std::vector<GoldenCase> cases{
      {"RudpBindHello", &*bind,
       RudpHeader{.flag = RudpFlag::Reliable,
                  .sessionId = 1,
                  .sessionGeneration = 2,
                  .transportEpoch = 0,
                  .sequence = 1,
                  .ack = 0,
                  .ackBits = 0,
                  .messageId = 22}},
      {"RudpBindAccepted", &*bind,
       RudpHeader{.flag = RudpFlag::Reliable,
                  .sessionId = 1,
                  .sessionGeneration = 2,
                  .transportEpoch = 3,
                  .sequence = 1,
                  .ack = 1,
                  .ackBits = 0,
                  .messageId = 23}},
      {"RudpHeartbeat", &*bind,
       RudpHeader{.flag = RudpFlag::Heartbeat,
                  .sessionId = 1,
                  .sessionGeneration = 2,
                  .transportEpoch = 3,
                  .sequence = 2,
                  .ack = 1,
                  .ackBits = 0,
                  .messageId = 24}},
      {"MoveIntent", &*movement,
       RudpHeader{.flag = RudpFlag::Unreliable,
                  .sessionId = 1,
                  .sessionGeneration = 2,
                  .transportEpoch = 3,
                  .sequence = 2,
                  .ack = 1,
                  .ackBits = 0,
                  .messageId = 25}},
      {"StateSnapshot", &*movement,
       RudpHeader{.flag = RudpFlag::Unreliable,
                  .sessionId = 1,
                  .sessionGeneration = 2,
                  .transportEpoch = 3,
                  .sequence = 3,
                  .ack = 2,
                  .ackBits = 1,
                  .messageId = 26}},
  };

  for (const auto &golden : cases) {
    const auto expected = goldenDatagram(*golden.contract, golden.semanticName);
    if (!expected.has_value() || expected->size() < kHeaderBytes) {
      return false;
    }
    const DecodedRudpDatagram decoded = RudpHeaderCodec::decode(*expected);
    const auto expectedPayload =
        std::span<const std::byte>{*expected}.subspan(kHeaderBytes);
    if (decoded.error != RudpHeaderError::None || !decoded.header.has_value() ||
        *decoded.header != golden.header ||
        !std::ranges::equal(decoded.payload, expectedPayload)) {
      return false;
    }
    const auto encoded =
        RudpHeaderCodec::encode(golden.header, decoded.payload);
    if (!encoded.has_value() || *encoded != *expected) {
      return false;
    }
  }
  return true;
}

bool malformedDatagramsAreRejected() {
  const auto movement = readText(LOOT_MOVEMENT_GOLDEN_PATH);
  const auto valid = movement.has_value()
                         ? goldenDatagram(*movement, "MoveIntent")
                         : std::nullopt;
  if (!valid.has_value()) {
    return false;
  }
  const auto rejects = [](const std::vector<std::byte> &datagram,
                          RudpHeaderError error) {
    return RudpHeaderCodec::decode(datagram).error == error;
  };

  auto tooShort = *valid;
  tooShort.resize(kHeaderBytes - 1);
  auto tooLarge = *valid;
  tooLarge.resize(1201, std::byte{0});
  auto magic = *valid;
  magic[0] = std::byte{0};
  auto version = *valid;
  version[4] = std::byte{2};
  auto headerBytes = *valid;
  headerBytes[7] = std::byte{47};
  auto combinedFlags = *valid;
  combinedFlags[5] = std::byte{3};
  auto reservedFlags = *valid;
  reservedFlags[5] = std::byte{0x80};
  auto payloadLength = *valid;
  payloadLength[43] = std::byte{0};
  auto checksum = *valid;
  checksum.back() ^= std::byte{1};

  return rejects(tooShort, RudpHeaderError::DatagramTooShort) &&
         rejects(tooLarge, RudpHeaderError::DatagramTooLarge) &&
         rejects(magic, RudpHeaderError::InvalidMagic) &&
         rejects(version, RudpHeaderError::UnsupportedVersion) &&
         rejects(headerBytes, RudpHeaderError::InvalidHeaderBytes) &&
         rejects(combinedFlags, RudpHeaderError::InvalidFlags) &&
         rejects(reservedFlags, RudpHeaderError::InvalidFlags) &&
         rejects(payloadLength, RudpHeaderError::PayloadLengthMismatch) &&
         rejects(checksum, RudpHeaderError::InvalidChecksum);
}

bool encodeEnforcesHeaderAndDatagramBounds() {
  RudpHeader header{.flag = RudpFlag::Unreliable,
                    .sessionId = 1,
                    .sessionGeneration = 2,
                    .transportEpoch = 3,
                    .sequence = 1,
                    .ack = 0,
                    .ackBits = 0,
                    .messageId = 25};
  const std::vector<std::byte> maximumPayload(1200 - kHeaderBytes);
  const auto maximum = RudpHeaderCodec::encode(header, maximumPayload);
  const std::vector<std::byte> oversizedPayload(maximumPayload.size() + 1);

  header.sequence = 0;
  const bool zeroSequenceRejected =
      !RudpHeaderCodec::encode(header, {}).has_value();
  header.sequence = 1;
  header.flag = RudpFlag::AckOnly;
  const bool malformedAckRejected =
      !RudpHeaderCodec::encode(header, {}).has_value();
  header.messageId = 0;
  const bool ackOnlyAccepted = RudpHeaderCodec::encode(header, {}).has_value();

  return maximum.has_value() && maximum->size() == 1200 &&
         !RudpHeaderCodec::encode(RudpHeader{.flag = RudpFlag::Unreliable,
                                             .sessionId = 1,
                                             .sessionGeneration = 2,
                                             .transportEpoch = 3,
                                             .sequence = 1,
                                             .ack = 0,
                                             .ackBits = 0,
                                             .messageId = 25},
                                  oversizedPayload)
              .has_value() &&
         zeroSequenceRejected && malformedAckRejected && ackOnlyAccepted;
}

bool halfRangeSerialArithmeticWraps() {
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  return isSequenceNewer(1, maximum) && isSequenceNewer(maximum, maximum - 1) &&
         !isSequenceNewer(maximum, 1) && !isSequenceNewer(7, 7) &&
         !isSequenceNewer(0x80000000U, 0);
}

} // namespace

int main() {
  return frozenGoldenVectorsRoundTrip() && malformedDatagramsAreRejected() &&
                 encodeEnforcesHeaderAndDatagramBounds() &&
                 halfRangeSerialArithmeticWraps()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
