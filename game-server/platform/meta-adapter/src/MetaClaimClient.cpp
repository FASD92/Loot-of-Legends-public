#include <lol/meta/MetaClaimClient.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace lol::meta {
namespace {

constexpr std::string_view kClaimPath = "/internal/v1/game-credentials/claim";
constexpr std::string_view kAudience = "loot-game-server-v1";

using StringObject = std::map<std::string, std::string, std::less<>>;

bool containsHeaderControl(std::string_view value) {
  return std::ranges::any_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte <= 0x20U || byte == 0x7fU;
  });
}

bool validCredential(std::string_view credential) {
  return credential.size() == 43 &&
         std::ranges::all_of(credential, [](char character) {
           return (character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '-' ||
                  character == '_';
         });
}

int hexValue(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

bool appendUtf8(std::string &output, std::uint32_t codePoint) {
  if (codePoint <= 0x7fU) {
    output.push_back(static_cast<char>(codePoint));
    return true;
  }
  if (codePoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    return true;
  }
  if (codePoint >= 0xd800U && codePoint <= 0xdfffU) {
    return false;
  }
  if (codePoint <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    return true;
  }
  if (codePoint <= 0x10ffffU) {
    output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    return true;
  }
  return false;
}

class StringObjectParser final {
public:
  explicit StringObjectParser(std::string_view input) : input_(input) {}

  [[nodiscard]] std::optional<StringObject> parse() {
    skipWhitespace();
    if (!consume('{')) {
      return std::nullopt;
    }
    skipWhitespace();
    StringObject values;
    if (consume('}')) {
      skipWhitespace();
      return position_ == input_.size() ? std::optional{std::move(values)}
                                        : std::nullopt;
    }

    while (true) {
      auto key = parseString();
      skipWhitespace();
      if (!key.has_value() || !consume(':')) {
        return std::nullopt;
      }
      skipWhitespace();
      auto value = parseString();
      if (!value.has_value() ||
          !values.emplace(std::move(*key), std::move(*value)).second) {
        return std::nullopt;
      }
      skipWhitespace();
      if (consume('}')) {
        skipWhitespace();
        return position_ == input_.size() ? std::optional{std::move(values)}
                                          : std::nullopt;
      }
      if (!consume(',')) {
        return std::nullopt;
      }
      skipWhitespace();
    }
  }

private:
  void skipWhitespace() noexcept {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n')) {
      ++position_;
    }
  }

  [[nodiscard]] bool consume(char expected) noexcept {
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  [[nodiscard]] std::optional<std::uint32_t> parseHexQuad() noexcept {
    if (input_.size() - position_ < 4) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t offset = 0; offset < 4; ++offset) {
      const int digit = hexValue(input_[position_ + offset]);
      if (digit < 0) {
        return std::nullopt;
      }
      value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    position_ += 4;
    return value;
  }

  [[nodiscard]] std::optional<std::string> parseString() {
    if (!consume('"')) {
      return std::nullopt;
    }
    std::string value;
    while (position_ < input_.size()) {
      const char character = input_[position_++];
      if (character == '"') {
        return value;
      }
      if (static_cast<unsigned char>(character) < 0x20U) {
        return std::nullopt;
      }
      if (character != '\\') {
        value.push_back(character);
        continue;
      }
      if (position_ >= input_.size()) {
        return std::nullopt;
      }
      const char escaped = input_[position_++];
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        value.push_back(escaped);
        break;
      case 'b':
        value.push_back('\b');
        break;
      case 'f':
        value.push_back('\f');
        break;
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      case 'u': {
        auto codePoint = parseHexQuad();
        if (!codePoint.has_value()) {
          return std::nullopt;
        }
        if (*codePoint >= 0xd800U && *codePoint <= 0xdbffU) {
          if (!consume('\\') || !consume('u')) {
            return std::nullopt;
          }
          const auto low = parseHexQuad();
          if (!low.has_value() || *low < 0xdc00U || *low > 0xdfffU) {
            return std::nullopt;
          }
          codePoint =
              0x10000U + ((*codePoint - 0xd800U) << 10U) + (*low - 0xdc00U);
        }
        if (!appendUtf8(value, *codePoint)) {
          return std::nullopt;
        }
        break;
      }
      default:
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  std::string_view input_;
  std::size_t position_{0};
};

bool validUtf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index++]);
    if (lead <= 0x7fU) {
      continue;
    }
    std::size_t continuationCount = 0;
    unsigned char secondMinimum = 0x80U;
    unsigned char secondMaximum = 0xbfU;
    if (lead >= 0xc2U && lead <= 0xdfU) {
      continuationCount = 1;
    } else if (lead >= 0xe0U && lead <= 0xefU) {
      continuationCount = 2;
      if (lead == 0xe0U) {
        secondMinimum = 0xa0U;
      } else if (lead == 0xedU) {
        secondMaximum = 0x9fU;
      }
    } else if (lead >= 0xf0U && lead <= 0xf4U) {
      continuationCount = 3;
      if (lead == 0xf0U) {
        secondMinimum = 0x90U;
      } else if (lead == 0xf4U) {
        secondMaximum = 0x8fU;
      }
    } else {
      return false;
    }
    if (text.size() - index < continuationCount) {
      return false;
    }
    const auto second = static_cast<unsigned char>(text[index]);
    if (second < secondMinimum || second > secondMaximum) {
      return false;
    }
    for (std::size_t offset = 1; offset < continuationCount; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      if (continuation < 0x80U || continuation > 0xbfU) {
        return false;
      }
    }
    index += continuationCount;
  }
  return true;
}

std::optional<std::array<std::uint8_t, 16>>
parseAccountId(std::string_view text) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return std::nullopt;
  }
  std::array<std::uint8_t, 16> bytes{};
  std::size_t byteIndex = 0;
  int highNibble = -1;
  for (const char character : text) {
    if (character == '-') {
      continue;
    }
    const int digit = hexValue(character);
    if (digit < 0) {
      return std::nullopt;
    }
    if (highNibble < 0) {
      highNibble = digit;
      continue;
    }
    if (byteIndex >= bytes.size()) {
      return std::nullopt;
    }
    bytes[byteIndex++] = static_cast<std::uint8_t>((highNibble << 4) | digit);
    highNibble = -1;
  }
  if (byteIndex != bytes.size() || highNibble >= 0) {
    return std::nullopt;
  }
  return bytes;
}

std::optional<ClaimOutcome> rejectionOutcome(int statusCode,
                                             const StringObject &body) {
  if (body.size() != 1) {
    return std::nullopt;
  }
  const auto code = body.find("code");
  if (code == body.end()) {
    return std::nullopt;
  }
  if (statusCode == 400 && code->second == "INVALID") {
    return ClaimOutcome::Invalid;
  }
  if (statusCode == 401 && code->second == "UNAUTHORIZED") {
    return ClaimOutcome::DependencyUnavailable;
  }
  if (statusCode == 409 && code->second == "ALREADY_CONSUMED") {
    return ClaimOutcome::AlreadyConsumed;
  }
  if (statusCode == 410 && code->second == "EXPIRED") {
    return ClaimOutcome::Expired;
  }
  if (statusCode == 422 && code->second == "WRONG_AUDIENCE") {
    return ClaimOutcome::WrongAudience;
  }
  return std::nullopt;
}

} // namespace

ClaimCompletion::ClaimCompletion(std::uint64_t connectionEpoch,
                                 std::uint64_t requestId, ClaimOutcome outcome,
                                 std::optional<ClaimedIdentity> identity)
    : connectionEpoch_(connectionEpoch), requestId_(requestId),
      outcome_(outcome), identity_(std::move(identity)) {}

std::uint64_t ClaimCompletion::connectionEpoch() const noexcept {
  return connectionEpoch_;
}

std::uint64_t ClaimCompletion::requestId() const noexcept { return requestId_; }

ClaimOutcome ClaimCompletion::outcome() const noexcept { return outcome_; }

const std::optional<ClaimedIdentity> &
ClaimCompletion::identity() const noexcept {
  return identity_;
}

MetaClaimClient::MetaClaimClient(MetaClaimClientConfig config,
                                 HttpsExchange exchange,
                                 CompletionSink completionSink)
    : config_(std::move(config)), exchange_(std::move(exchange)),
      completionSink_(std::move(completionSink)) {
  if (!config_.claimUrl.starts_with("https://") ||
      !config_.claimUrl.ends_with(kClaimPath) ||
      containsHeaderControl(config_.claimUrl) ||
      config_.serviceCredential.empty() ||
      containsHeaderControl(config_.serviceCredential) ||
      config_.timeout <= std::chrono::milliseconds::zero() ||
      config_.maxOutstanding == 0 || config_.maxResponseBytes == 0 ||
      !exchange_ || !completionSink_) {
    throw std::invalid_argument{"invalid Meta claim client configuration"};
  }
  worker_ = std::thread{[this] { run(); }};
}

MetaClaimClient::~MetaClaimClient() { stop(); }

SubmitStatus MetaClaimClient::submit(ClaimSubmission submission) {
  if (submission.connectionEpoch == 0 || submission.requestId == 0 ||
      !validCredential(submission.credential)) {
    return SubmitStatus::InvalidRequest;
  }
  {
    std::lock_guard lock{mutex_};
    if (stopping_) {
      return SubmitStatus::Stopped;
    }
    if (outstanding_ >= config_.maxOutstanding) {
      return SubmitStatus::Overloaded;
    }
    queue_.push_back(std::move(submission));
    ++outstanding_;
  }
  workAvailable_.notify_one();
  return SubmitStatus::Accepted;
}

bool MetaClaimClient::waitUntilIdle(
    std::chrono::milliseconds timeout) noexcept {
  std::unique_lock lock{mutex_};
  return idle_.wait_for(lock, timeout, [this] { return outstanding_ == 0; });
}

void MetaClaimClient::stop() noexcept {
  {
    std::lock_guard lock{mutex_};
    stopping_ = true;
  }
  workAvailable_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void MetaClaimClient::run() {
  while (true) {
    std::optional<ClaimSubmission> submission;
    {
      std::unique_lock lock{mutex_};
      workAvailable_.wait(lock,
                          [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        return;
      }
      submission.emplace(std::move(queue_.front()));
      queue_.pop_front();
    }

    HttpsResult result{
        .status = HttpsStatus::NetworkFailure, .statusCode = 0, .body = {}};
    try {
      result = exchange_(makeRequest(*submission), config_.timeout);
    } catch (...) {
      result = HttpsResult{
          .status = HttpsStatus::NetworkFailure, .statusCode = 0, .body = {}};
    }
    completionSink_(decode(*submission, result));

    {
      std::lock_guard lock{mutex_};
      --outstanding_;
    }
    idle_.notify_all();
  }
}

HttpsRequest
MetaClaimClient::makeRequest(const ClaimSubmission &submission) const {
  return HttpsRequest{
      .method = "POST",
      .url = config_.claimUrl,
      .authorization = "Bearer " + config_.serviceCredential,
      .contentType = "application/json",
      .body = "{\"credential\":\"" + submission.credential +
              "\",\"audience\":\"" + std::string{kAudience} + "\"}",
      .maxResponseBytes = config_.maxResponseBytes,
  };
}

ClaimCompletion MetaClaimClient::decode(const ClaimSubmission &submission,
                                        const HttpsResult &result) const {
  const auto complete = [&submission](ClaimOutcome outcome,
                                      std::optional<ClaimedIdentity> identity =
                                          std::nullopt) {
    return ClaimCompletion{submission.connectionEpoch, submission.requestId,
                           outcome, std::move(identity)};
  };

  if (result.status != HttpsStatus::Response || result.statusCode >= 500) {
    return complete(ClaimOutcome::DependencyUnavailable);
  }
  if (result.body.size() > config_.maxResponseBytes) {
    return complete(ClaimOutcome::MalformedResponse);
  }
  const auto body = StringObjectParser{result.body}.parse();
  if (!body.has_value()) {
    return complete(ClaimOutcome::MalformedResponse);
  }
  if (result.statusCode == 200) {
    if (body->size() != 2) {
      return complete(ClaimOutcome::MalformedResponse);
    }
    const auto account = body->find("accountId");
    const auto nickname = body->find("nickname");
    if (account == body->end() || nickname == body->end() ||
        nickname->second.empty() || !validUtf8(nickname->second)) {
      return complete(ClaimOutcome::MalformedResponse);
    }
    auto accountId = parseAccountId(account->second);
    if (!accountId.has_value()) {
      return complete(ClaimOutcome::MalformedResponse);
    }
    return complete(ClaimOutcome::Claimed,
                    ClaimedIdentity{std::move(*accountId), nickname->second});
  }
  const auto rejection = rejectionOutcome(result.statusCode, *body);
  return complete(rejection.value_or(ClaimOutcome::MalformedResponse));
}

} // namespace lol::meta
