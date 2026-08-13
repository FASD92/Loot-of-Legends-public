#include <lol/meta/MetaSettlementClient.hpp>

#include <lol/settlement/SettlementIntent.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lol::meta {
namespace {

constexpr std::size_t kSettlementIdOffset = 20u;
constexpr std::size_t kSettlementIdBytes = 16u;
constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                       '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
constexpr std::array<char, 64> kBase64 = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

bool containsControl(std::string_view value) {
  return std::ranges::any_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte <= 0x20u || byte == 0x7fu;
  });
}

std::string hex(std::span<const std::uint8_t> bytes) {
  std::string text;
  text.reserve(bytes.size() * 2u);
  for (const auto byte : bytes) {
    text.push_back(kHex[byte >> 4u]);
    text.push_back(kHex[byte & 0x0fu]);
  }
  return text;
}

std::string base64(std::span<const std::uint8_t> bytes) {
  std::string text;
  text.reserve(((bytes.size() + 2u) / 3u) * 4u);
  std::size_t offset{};
  while (bytes.size() - offset >= 3u) {
    const auto value = (static_cast<std::uint32_t>(bytes[offset]) << 16u) |
                       (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
                       static_cast<std::uint32_t>(bytes[offset + 2u]);
    text.push_back(kBase64[(value >> 18u) & 0x3fu]);
    text.push_back(kBase64[(value >> 12u) & 0x3fu]);
    text.push_back(kBase64[(value >> 6u) & 0x3fu]);
    text.push_back(kBase64[value & 0x3fu]);
    offset += 3u;
  }
  const auto remaining = bytes.size() - offset;
  if (remaining == 1u) {
    const auto value = static_cast<std::uint32_t>(bytes[offset]) << 16u;
    text.push_back(kBase64[(value >> 18u) & 0x3fu]);
    text.push_back(kBase64[(value >> 12u) & 0x3fu]);
    text.append("==");
  } else if (remaining == 2u) {
    const auto value = (static_cast<std::uint32_t>(bytes[offset]) << 16u) |
                       (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u);
    text.push_back(kBase64[(value >> 18u) & 0x3fu]);
    text.push_back(kBase64[(value >> 12u) & 0x3fu]);
    text.push_back(kBase64[(value >> 6u) & 0x3fu]);
    text.push_back('=');
  }
  return text;
}

std::optional<std::string> stringField(std::string_view body,
                                       std::string_view field) {
  const std::string marker = '"' + std::string{field} + '"';
  const auto fieldPosition = body.find(marker);
  if (fieldPosition == std::string_view::npos ||
      body.find(marker, fieldPosition + marker.size()) !=
          std::string_view::npos) {
    return std::nullopt;
  }
  auto position = fieldPosition + marker.size();
  while (position < body.size() &&
         std::isspace(static_cast<unsigned char>(body[position])) != 0) {
    ++position;
  }
  if (position >= body.size() || body[position++] != ':') {
    return std::nullopt;
  }
  while (position < body.size() &&
         std::isspace(static_cast<unsigned char>(body[position])) != 0) {
    ++position;
  }
  if (position >= body.size() || body[position++] != '"') {
    return std::nullopt;
  }
  std::string value;
  while (position < body.size() && body[position] != '"') {
    const auto character = static_cast<unsigned char>(body[position++]);
    if (character < 0x20u || character == '\\') {
      return std::nullopt;
    }
    value.push_back(static_cast<char>(character));
  }
  if (position >= body.size()) {
    return std::nullopt;
  }
  return value;
}

bool errorCode(const HttpsResult &result, std::string_view code) {
  return stringField(result.body, "code") == code;
}

} // namespace

MetaSettlementClient::MetaSettlementClient(MetaSettlementClientConfig config,
                                           HttpsExchange exchange)
    : config_(std::move(config)), exchange_(std::move(exchange)) {
  if (!config_.settlementsUrl.starts_with("https://") ||
      containsControl(config_.settlementsUrl) ||
      config_.serviceCredential.empty() ||
      containsControl(config_.serviceCredential) ||
      config_.timeout <= std::chrono::milliseconds::zero() ||
      config_.maxResponseBytes == 0u || !exchange_) {
    throw std::invalid_argument("invalid Meta settlement client config");
  }
}

settlement::MetaPublishOutcome MetaSettlementClient::publish(
    const settlement::DurableSettlementIntent &intent) {
  const auto id = settlementId(intent);
  if (id.empty()) {
    return settlement::MetaPublishOutcome::Rejected;
  }
  const auto hash =
      settlement::hashCanonicalPayload(intent.canonicalPayload).bytes();
  HttpsRequest request{
      .method = "POST",
      .url = config_.settlementsUrl,
      .authorization = "Bearer " + config_.serviceCredential,
      .contentType = "application/json",
      .body = "{\"settlementId\":\"" + id + "\",\"payloadHash\":\"" +
              hex(hash) + "\",\"canonicalPayload\":\"" +
              base64(intent.canonicalPayload) + "\"}",
      .maxResponseBytes = config_.maxResponseBytes,
  };
  const auto result = exchange_(std::move(request), config_.timeout);
  if (result.status != HttpsStatus::Response) {
    return settlement::MetaPublishOutcome::ResponseLost;
  }
  if (result.body.size() > config_.maxResponseBytes) {
    return settlement::MetaPublishOutcome::Rejected;
  }
  if (result.statusCode == 200 &&
      stringField(result.body, "settlementId") == id) {
    const auto statusValue = stringField(result.body, "status");
    if (statusValue == "AcceptedPending") {
      return settlement::MetaPublishOutcome::AcceptedPending;
    }
    if (statusValue == "Applied") {
      return settlement::MetaPublishOutcome::Applied;
    }
  }
  if (result.statusCode == 409 && errorCode(result, "CONFLICT")) {
    return settlement::MetaPublishOutcome::Conflict;
  }
  if (result.statusCode >= 500 && result.statusCode <= 599) {
    return settlement::MetaPublishOutcome::Retryable;
  }
  return settlement::MetaPublishOutcome::Rejected;
}

settlement::MetaStatusOutcome MetaSettlementClient::status(
    const settlement::DurableSettlementIntent &intent) {
  const auto id = settlementId(intent);
  if (id.empty()) {
    return settlement::MetaStatusOutcome::Rejected;
  }
  HttpsRequest request{
      .method = "GET",
      .url = config_.settlementsUrl + "/" + id,
      .authorization = "Bearer " + config_.serviceCredential,
      .contentType = {},
      .body = {},
      .maxResponseBytes = config_.maxResponseBytes,
  };
  const auto result = exchange_(std::move(request), config_.timeout);
  if (result.status != HttpsStatus::Response ||
      (result.statusCode >= 500 && result.statusCode <= 599)) {
    return settlement::MetaStatusOutcome::Retryable;
  }
  if (result.body.size() > config_.maxResponseBytes) {
    return settlement::MetaStatusOutcome::Rejected;
  }
  if (result.statusCode == 200 &&
      stringField(result.body, "settlementId") == id) {
    const auto statusValue = stringField(result.body, "status");
    if (statusValue == "AcceptedPending") {
      return settlement::MetaStatusOutcome::AcceptedPending;
    }
    if (statusValue == "Applied") {
      return settlement::MetaStatusOutcome::Applied;
    }
  }
  if (result.statusCode == 404 && errorCode(result, "NOT_FOUND")) {
    return settlement::MetaStatusOutcome::NotFound;
  }
  return settlement::MetaStatusOutcome::Rejected;
}

std::string MetaSettlementClient::settlementId(
    const settlement::DurableSettlementIntent &intent) const {
  if (intent.canonicalPayload.size() <
      kSettlementIdOffset + kSettlementIdBytes) {
    return {};
  }
  return hex(std::span{intent.canonicalPayload}.subspan(kSettlementIdOffset,
                                                        kSettlementIdBytes));
}

} // namespace lol::meta
