#include <lol/settlement/SettlementPublisher.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace lol::settlement {
namespace {

using namespace std::chrono_literals;

constexpr std::array<std::chrono::milliseconds, 8> kRetryBase = {
    250ms, 500ms, 1'000ms, 2'000ms, 4'000ms, 8'000ms, 15'000ms, 30'000ms};
constexpr auto kRetryCap = 30'000ms;

} // namespace

SettlementPublisher::SettlementPublisher(SettlementOutboxPort &outbox,
                                         SettlementMetaPort &meta) noexcept
    : outbox_(outbox), meta_(meta) {}

PublisherStepResult
SettlementPublisher::step(std::chrono::steady_clock::time_point now) {
  if (blocked_) {
    return {.code = PublisherStepCode::Blocked};
  }
  if (waiting_ && now < nextAttempt_) {
    return {
        .code = PublisherStepCode::Waiting,
        .retryAfter = std::chrono::duration_cast<std::chrono::milliseconds>(
            nextAttempt_ - now),
    };
  }
  waiting_ = false;

  if (compactionPending_) {
    if (outbox_.compact()) {
      compactionPending_ = false;
      retryAttempt_ = 0u;
      return {.code = PublisherStepCode::Retired};
    }
    return scheduleRetry(now);
  }

  if (!batch_.has_value()) {
    auto loaded = outbox_.nextUnretired();
    intentIndex_ = 0u;
    phase_ = IntentPhase::Publish;
    if (loaded.status == OutboxLoadStatus::Unavailable) {
      return scheduleRetry(now);
    }
    if (loaded.status == OutboxLoadStatus::Empty) {
      retryAttempt_ = 0u;
      return {.code = PublisherStepCode::Idle};
    }
    batch_ = std::move(loaded.batch);
    retryAttempt_ = 0u;
    if (!batch_.has_value()) {
      blocked_ = true;
      return {.code = PublisherStepCode::Blocked};
    }
    if (batch_->commitSequence == 0u || batch_->intents.empty()) {
      blocked_ = true;
      return {.code = PublisherStepCode::Blocked};
    }
  }

  if (intentIndex_ == batch_->intents.size()) {
    const auto retirement =
        outbox_.retire(batch_->batchId, batch_->commitSequence);
    if (retirement == OutboxRetireResult::Unavailable) {
      return scheduleRetry(now);
    }
    if (retirement == OutboxRetireResult::NotFound) {
      blocked_ = true;
      return {.code = PublisherStepCode::Blocked};
    }
    batch_.reset();
    intentIndex_ = 0u;
    retryAttempt_ = 0u;
    compactionPending_ = true;
    if (outbox_.compact()) {
      compactionPending_ = false;
      return {.code = PublisherStepCode::Retired};
    }
    return scheduleRetry(now);
  }

  const auto &intent = batch_->intents[intentIndex_];
  if (phase_ == IntentPhase::Publish) {
    switch (meta_.publish(intent)) {
    case MetaPublishOutcome::Applied:
      return advanceIntent();
    case MetaPublishOutcome::AcceptedPending:
    case MetaPublishOutcome::ResponseLost:
      phase_ = IntentPhase::ReconcileStatus;
      retryAttempt_ = 0u;
      return scheduleRetry(now);
    case MetaPublishOutcome::Retryable:
      return scheduleRetry(now);
    case MetaPublishOutcome::Conflict:
    case MetaPublishOutcome::Rejected:
      blocked_ = true;
      return {.code = PublisherStepCode::Blocked};
    }
  }

  switch (meta_.status(intent)) {
  case MetaStatusOutcome::Applied:
    return advanceIntent();
  case MetaStatusOutcome::AcceptedPending:
  case MetaStatusOutcome::Retryable:
    return scheduleRetry(now);
  case MetaStatusOutcome::NotFound:
    phase_ = IntentPhase::Publish;
    return scheduleRetry(now);
  case MetaStatusOutcome::Rejected:
    blocked_ = true;
    return {.code = PublisherStepCode::Blocked};
  }
  blocked_ = true;
  return {.code = PublisherStepCode::Blocked};
}

PublisherStepResult
SettlementPublisher::scheduleRetry(std::chrono::steady_clock::time_point now) {
  const auto delay = retryDelay();
  ++retryAttempt_;
  waiting_ = true;
  nextAttempt_ = now + delay;
  return {.code = PublisherStepCode::Retrying, .retryAfter = delay};
}

PublisherStepResult SettlementPublisher::advanceIntent() noexcept {
  ++intentIndex_;
  retryAttempt_ = 0u;
  phase_ = IntentPhase::Publish;
  return {.code = PublisherStepCode::Progress};
}

std::chrono::milliseconds SettlementPublisher::retryDelay() const noexcept {
  const auto base = kRetryBase[std::min(retryAttempt_, kRetryBase.size() - 1u)];
  const auto window = base / 8;
  if (window.count() == 0 || base == kRetryCap || !batch_.has_value()) {
    return base;
  }

  std::uint64_t seed = 1469598103934665603ULL;
  for (const auto byte : batch_->batchId.bytes()) {
    seed ^= byte;
    seed *= 1099511628211ULL;
  }
  seed ^= static_cast<std::uint64_t>(intentIndex_ + 1u);
  seed *= 1099511628211ULL;
  seed ^= static_cast<std::uint64_t>(retryAttempt_ + 1u);
  const auto jitter =
      std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
          seed % static_cast<std::uint64_t>(window.count() + 1))};
  return std::min(kRetryCap, base + jitter);
}

} // namespace lol::settlement
