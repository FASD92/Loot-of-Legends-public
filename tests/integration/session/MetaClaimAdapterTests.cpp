#include "AuthClaimCoordinator.hpp"

#include <lol/meta/MetaClaimClient.hpp>
#include <lol/session/SessionRegistry.hpp>
#include <lol/shared/Identifiers.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::meta::ClaimCompletion;
using lol::meta::ClaimOutcome;
using lol::meta::ClaimSubmission;
using lol::meta::HttpsRequest;
using lol::meta::HttpsResult;
using lol::meta::HttpsStatus;
using lol::meta::MetaClaimClient;
using lol::meta::MetaClaimClientConfig;
using lol::meta::SubmitStatus;

constexpr auto kClaimUrl =
    "https://meta.test/internal/v1/game-credentials/claim";
constexpr auto kServiceCredential = "test-only-service-placeholder";
constexpr auto kGameCredential = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

MetaClaimClientConfig config(std::size_t maxOutstanding = 4) {
  return MetaClaimClientConfig{
      .claimUrl = kClaimUrl,
      .serviceCredential = kServiceCredential,
      .timeout = 50ms,
      .maxOutstanding = maxOutstanding,
      .maxResponseBytes = 1024,
  };
}

ClaimSubmission submission(std::uint64_t epoch, std::uint64_t requestId) {
  return ClaimSubmission{
      .connectionEpoch = epoch,
      .requestId = requestId,
      .credential = kGameCredential,
  };
}

class CompletionCollector final {
public:
  void add(ClaimCompletion completion) {
    std::lock_guard lock{mutex_};
    completions_.push_back(std::move(completion));
  }

  std::vector<ClaimCompletion> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(completions_, {});
  }

private:
  std::mutex mutex_;
  std::vector<ClaimCompletion> completions_;
};

bool validResponseRunsOffCallerAndCreatesOneSession() {
  CompletionCollector collector;
  std::optional<HttpsRequest> observedRequest;
  std::chrono::milliseconds observedTimeout{};
  std::thread::id exchangeThread;
  const std::thread::id callerThread = std::this_thread::get_id();
  lol::session::SessionRegistry registry;
  lol::app::AuthClaimCoordinator coordinator{registry};
  const ClaimSubmission claim = submission(11, 101);
  if (!coordinator.beginClaim(claim.connectionEpoch,
                              lol::shared::RequestId{claim.requestId})) {
    return false;
  }

  MetaClaimClient client{
      config(),
      [&observedRequest, &observedTimeout, &exchangeThread](
          HttpsRequest request, std::chrono::milliseconds timeout) {
        observedRequest = std::move(request);
        observedTimeout = timeout;
        exchangeThread = std::this_thread::get_id();
        return HttpsResult{
            .status = HttpsStatus::Response,
            .statusCode = 200,
            .body =
                R"({"nickname":"player-one","accountId":"00000000-0000-4000-8000-000000000001"})",
        };
      },
      [&collector](ClaimCompletion completion) {
        collector.add(std::move(completion));
      }};

  if (client.submit(claim) != SubmitStatus::Accepted ||
      !client.waitUntilIdle(1s)) {
    return false;
  }
  auto completions = collector.take();
  if (completions.size() != 1 ||
      completions.front().outcome() != ClaimOutcome::Claimed ||
      !completions.front().identity().has_value() ||
      completions.front().identity()->nickname != "player-one") {
    return false;
  }

  const auto applied = coordinator.apply(completions.front());
  return applied.kind == lol::app::AppliedClaimKind::Accepted &&
         applied.authenticated.has_value() &&
         registry.activeSessionCount() == 1 && observedRequest.has_value() &&
         observedRequest->method == "POST" &&
         observedRequest->url == kClaimUrl &&
         observedRequest->authorization ==
             std::string{"Bearer "} + kServiceCredential &&
         observedRequest->contentType == "application/json" &&
         observedRequest->maxResponseBytes == 1024 &&
         observedRequest->body ==
             std::string{R"({"credential":")"} + kGameCredential +
                 R"(","audience":"loot-game-server-v1"})" &&
         observedTimeout == 50ms && exchangeThread != callerThread;
}

bool closedConnectionRejectsLateCompletion() {
  CompletionCollector collector;
  std::mutex gateMutex;
  std::condition_variable gateChanged;
  bool exchangeStarted = false;
  bool releaseExchange = false;
  lol::session::SessionRegistry registry;
  lol::app::AuthClaimCoordinator coordinator{registry};
  const ClaimSubmission claim = submission(12, 102);
  if (!coordinator.beginClaim(claim.connectionEpoch,
                              lol::shared::RequestId{claim.requestId})) {
    return false;
  }

  MetaClaimClient client{
      config(),
      [&gateMutex, &gateChanged, &exchangeStarted,
       &releaseExchange](HttpsRequest, std::chrono::milliseconds) {
        std::unique_lock lock{gateMutex};
        exchangeStarted = true;
        gateChanged.notify_all();
        gateChanged.wait(lock, [&releaseExchange] { return releaseExchange; });
        return HttpsResult{
            .status = HttpsStatus::Response,
            .statusCode = 200,
            .body =
                R"({"accountId":"00000000-0000-4000-8000-000000000001","nickname":"player-one"})",
        };
      },
      [&collector](ClaimCompletion completion) {
        collector.add(std::move(completion));
      }};

  if (client.submit(claim) != SubmitStatus::Accepted) {
    return false;
  }
  {
    std::unique_lock lock{gateMutex};
    if (!gateChanged.wait_for(lock, 1s,
                              [&exchangeStarted] { return exchangeStarted; })) {
      releaseExchange = true;
      gateChanged.notify_all();
      return false;
    }
  }

  const bool closed = coordinator.closeConnection(claim.connectionEpoch);
  {
    std::lock_guard lock{gateMutex};
    releaseExchange = true;
  }
  gateChanged.notify_all();
  if (!closed || !client.waitUntilIdle(1s)) {
    return false;
  }

  auto completions = collector.take();
  return completions.size() == 1 &&
         coordinator.apply(completions.front()).kind ==
             lol::app::AppliedClaimKind::Stale &&
         registry.activeSessionCount() == 0;
}

bool timeoutServerErrorAndMalformedResponseStayBounded() {
  CompletionCollector collector;
  std::vector<HttpsResult> responses{
      HttpsResult{.status = HttpsStatus::Timeout, .statusCode = 0, .body = {}},
      HttpsResult{.status = HttpsStatus::Response,
                  .statusCode = 503,
                  .body = R"({"code":"DEPENDENCY_UNAVAILABLE"})"},
      HttpsResult{.status = HttpsStatus::Response,
                  .statusCode = 200,
                  .body = R"({"accountId":"not-a-uuid","nickname":"player"})"},
  };
  std::size_t nextResponse = 0;
  MetaClaimClient client{
      config(),
      [&responses, &nextResponse](HttpsRequest, std::chrono::milliseconds) {
        return responses.at(nextResponse++);
      },
      [&collector](ClaimCompletion completion) {
        collector.add(std::move(completion));
      }};

  if (client.submit(submission(21, 201)) != SubmitStatus::Accepted ||
      client.submit(submission(22, 202)) != SubmitStatus::Accepted ||
      client.submit(submission(23, 203)) != SubmitStatus::Accepted ||
      !client.waitUntilIdle(1s)) {
    return false;
  }
  const auto completions = collector.take();
  return completions.size() == 3 &&
         completions[0].outcome() == ClaimOutcome::DependencyUnavailable &&
         completions[1].outcome() == ClaimOutcome::DependencyUnavailable &&
         completions[2].outcome() == ClaimOutcome::MalformedResponse;
}

bool outstandingQueueRejectsOverload() {
  CompletionCollector collector;
  std::mutex gateMutex;
  std::condition_variable gateChanged;
  bool exchangeStarted = false;
  bool releaseExchange = false;
  MetaClaimClient client{
      config(1),
      [&gateMutex, &gateChanged, &exchangeStarted,
       &releaseExchange](HttpsRequest, std::chrono::milliseconds) {
        std::unique_lock lock{gateMutex};
        exchangeStarted = true;
        gateChanged.notify_all();
        gateChanged.wait(lock, [&releaseExchange] { return releaseExchange; });
        return HttpsResult{
            .status = HttpsStatus::Timeout, .statusCode = 0, .body = {}};
      },
      [&collector](ClaimCompletion completion) {
        collector.add(std::move(completion));
      }};

  if (client.submit(submission(31, 301)) != SubmitStatus::Accepted) {
    return false;
  }
  {
    std::unique_lock lock{gateMutex};
    if (!gateChanged.wait_for(lock, 1s,
                              [&exchangeStarted] { return exchangeStarted; })) {
      releaseExchange = true;
      gateChanged.notify_all();
      return false;
    }
  }
  const SubmitStatus overloaded = client.submit(submission(32, 302));
  {
    std::lock_guard lock{gateMutex};
    releaseExchange = true;
  }
  gateChanged.notify_all();
  return overloaded == SubmitStatus::Overloaded && client.waitUntilIdle(1s) &&
         collector.take().size() == 1;
}

bool emptyServiceCredentialFailsClosed() {
  MetaClaimClientConfig invalid = config();
  invalid.serviceCredential.clear();
  try {
    MetaClaimClient client{std::move(invalid),
                           [](HttpsRequest, std::chrono::milliseconds) {
                             return HttpsResult{.status =
                                                    HttpsStatus::NetworkFailure,
                                                .statusCode = 0,
                                                .body = {}};
                           },
                           [](ClaimCompletion) {}};
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

} // namespace

int main() {
  if (!validResponseRunsOffCallerAndCreatesOneSession()) {
    return 1;
  }
  if (!closedConnectionRejectsLateCompletion()) {
    return 2;
  }
  if (!timeoutServerErrorAndMalformedResponseStayBounded()) {
    return 3;
  }
  if (!outstandingQueueRejectsOverload()) {
    return 4;
  }
  if (!emptyServiceCredentialFailsClosed()) {
    return 5;
  }
  return EXIT_SUCCESS;
}
