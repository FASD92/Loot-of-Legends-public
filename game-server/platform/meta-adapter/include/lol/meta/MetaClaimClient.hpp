#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace lol::meta {

struct MetaClaimClientConfig final {
  std::string claimUrl;
  std::string serviceCredential;
  std::chrono::milliseconds timeout;
  std::size_t maxOutstanding;
  std::size_t maxResponseBytes;
};

struct ClaimSubmission final {
  std::uint64_t connectionEpoch;
  std::uint64_t requestId;
  std::string credential;
};

struct ClaimedIdentity final {
  std::array<std::uint8_t, 16> accountId;
  std::string nickname;
};

enum class ClaimOutcome {
  Claimed,
  Invalid,
  AlreadyConsumed,
  Expired,
  WrongAudience,
  DependencyUnavailable,
  MalformedResponse,
};

class ClaimCompletion final {
public:
  ClaimCompletion(std::uint64_t connectionEpoch, std::uint64_t requestId,
                  ClaimOutcome outcome,
                  std::optional<ClaimedIdentity> identity);

  [[nodiscard]] std::uint64_t connectionEpoch() const noexcept;
  [[nodiscard]] std::uint64_t requestId() const noexcept;
  [[nodiscard]] ClaimOutcome outcome() const noexcept;
  [[nodiscard]] const std::optional<ClaimedIdentity> &identity() const noexcept;

private:
  std::uint64_t connectionEpoch_;
  std::uint64_t requestId_;
  ClaimOutcome outcome_;
  std::optional<ClaimedIdentity> identity_;
};

struct HttpsRequest final {
  std::string method;
  std::string url;
  std::string authorization;
  std::string contentType;
  std::string body;
  std::size_t maxResponseBytes;
};

enum class HttpsStatus { Response, Timeout, NetworkFailure, PolicyFailure };

struct HttpsResult final {
  HttpsStatus status;
  int statusCode;
  std::string body;
};

using HttpsExchange =
    std::function<HttpsResult(HttpsRequest, std::chrono::milliseconds)>;
using CompletionSink = std::function<void(ClaimCompletion)>;

enum class SubmitStatus { Accepted, Overloaded, InvalidRequest, Stopped };

class MetaClaimClient final {
public:
  MetaClaimClient(MetaClaimClientConfig config, HttpsExchange exchange,
                  CompletionSink completionSink);
  ~MetaClaimClient();

  MetaClaimClient(const MetaClaimClient &) = delete;
  MetaClaimClient &operator=(const MetaClaimClient &) = delete;

  [[nodiscard]] SubmitStatus submit(ClaimSubmission submission);
  [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout) noexcept;
  void stop() noexcept;

private:
  void run();
  [[nodiscard]] HttpsRequest
  makeRequest(const ClaimSubmission &submission) const;
  [[nodiscard]] ClaimCompletion decode(const ClaimSubmission &submission,
                                       const HttpsResult &result) const;

  MetaClaimClientConfig config_;
  HttpsExchange exchange_;
  CompletionSink completionSink_;
  std::mutex mutex_;
  std::condition_variable workAvailable_;
  std::condition_variable idle_;
  std::deque<ClaimSubmission> queue_;
  std::size_t outstanding_{0};
  bool stopping_{false};
  std::thread worker_;
};

} // namespace lol::meta
