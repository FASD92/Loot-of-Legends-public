#include <lol/meta/MetaSettlementClient.hpp>
#include <lol/settlement/SettlementPublisher.hpp>
#include <lol/settlement_storage/SegmentJournal.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#ifndef LOOT_OUTBOX_GOLDEN_DIR
#error "LOOT_OUTBOX_GOLDEN_DIR must name the outbox journal golden directory"
#endif

namespace {

using namespace std::chrono_literals;
using lol::settlement::DurableSettlementBatch;
using lol::settlement::DurableSettlementIntent;
using lol::settlement::MetaPublishOutcome;
using lol::settlement::MetaStatusOutcome;
using lol::settlement::OutboxLoadResult;
using lol::settlement::OutboxLoadStatus;
using lol::settlement::OutboxRetireResult;
using lol::settlement::PublisherStepCode;
using lol::settlement::SettlementBatchId;
using lol::settlement::SettlementMetaPort;
using lol::settlement::SettlementOutboxPort;
using lol::settlement::SettlementPublisher;
using lol::settlement_storage::AppendBatch;
using lol::settlement_storage::AppendResultStatus;
using lol::settlement_storage::JournalRecoveryStatus;
using lol::settlement_storage::SegmentJournal;

class TempDirectory final {
public:
  TempDirectory() {
    const std::string base =
        (std::filesystem::temp_directory_path() / "loot-publisher-XXXXXX")
            .string();
    std::vector<char> pattern(base.begin(), base.end());
    pattern.push_back('\0');
    path_ = ::mkdtemp(pattern.data());
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

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

AppendBatch goldenBatch(std::uint8_t discriminator = 0x62u) {
  const auto valid = readHex("outbox-journal-valid.hex");
  constexpr std::size_t firstPayloadOffset = 56u;
  constexpr std::size_t payloadLength = 114u;
  constexpr std::size_t secondPayloadOffset = 174u + 56u;
  lol::settlement_storage::JournalBatchId id = {
      discriminator, 0x0b, 0x39, 0xc0, 0xdb, 0x27, 0x93, 0x37,
      0x4d,          0x8a, 0x5e, 0xf7, 0xe3, 0x44, 0x15, 0x36};
  return {
      .batchId = id,
      .canonicalIntents =
          {std::vector<std::uint8_t>(
               valid.begin() + static_cast<std::ptrdiff_t>(firstPayloadOffset),
               valid.begin() + static_cast<std::ptrdiff_t>(firstPayloadOffset +
                                                           payloadLength)),
           std::vector<std::uint8_t>(
               valid.begin() + static_cast<std::ptrdiff_t>(secondPayloadOffset),
               valid.begin() + static_cast<std::ptrdiff_t>(secondPayloadOffset +
                                                           payloadLength))},
  };
}

DurableSettlementBatch durableBatch() {
  const auto batch = goldenBatch();
  return {
      .batchId = SettlementBatchId{batch.batchId},
      .commitSequence = 3u,
      .intents = {DurableSettlementIntent{batch.canonicalIntents[0]},
                  DurableSettlementIntent{batch.canonicalIntents[1]}},
  };
}

class FakeOutbox final : public SettlementOutboxPort {
public:
  OutboxLoadResult nextUnretired() override {
    std::lock_guard lock{mutex_};
    ++loads;
    if (unavailable) {
      return {.status = OutboxLoadStatus::Unavailable, .batch = std::nullopt};
    }
    return retired ? OutboxLoadResult{.status = OutboxLoadStatus::Empty,
                                      .batch = std::nullopt}
                   : OutboxLoadResult{.status = OutboxLoadStatus::Loaded,
                                      .batch = batch};
  }

  OutboxRetireResult retire(const SettlementBatchId &batchId,
                            std::uint64_t commitSequence) override {
    std::lock_guard lock{mutex_};
    if (batchId != batch.batchId || commitSequence != batch.commitSequence) {
      return OutboxRetireResult::Unavailable;
    }
    if (retired) {
      return OutboxRetireResult::AlreadyRetired;
    }
    retired = true;
    ++retireCalls;
    return OutboxRetireResult::Retired;
  }

  bool compact() override {
    std::lock_guard lock{mutex_};
    ++compactCalls;
    return true;
  }

  DurableSettlementBatch batch{durableBatch()};
  std::size_t loads{};
  std::size_t retireCalls{};
  std::size_t compactCalls{};
  bool retired{};
  bool unavailable{};

private:
  std::mutex mutex_;
};

class ScriptedMeta final : public SettlementMetaPort {
public:
  MetaPublishOutcome publish(const DurableSettlementIntent &) override {
    ++publishCalls;
    if (publishResults.empty()) {
      return MetaPublishOutcome::Applied;
    }
    const auto result = publishResults.front();
    publishResults.erase(publishResults.begin());
    return result;
  }

  MetaStatusOutcome status(const DurableSettlementIntent &) override {
    ++statusCalls;
    if (statusResults.empty()) {
      return MetaStatusOutcome::Applied;
    }
    const auto result = statusResults.front();
    statusResults.erase(statusResults.begin());
    return result;
  }

  std::vector<MetaPublishOutcome> publishResults;
  std::vector<MetaStatusOutcome> statusResults;
  std::size_t publishCalls{};
  std::size_t statusCalls{};
};

bool retriesWithFakeClockFrom250MillisecondsTo30Seconds() {
  FakeOutbox outbox;
  ScriptedMeta meta;
  meta.publishResults.assign(9u, MetaPublishOutcome::Retryable);
  meta.publishResults.push_back(MetaPublishOutcome::Applied);
  SettlementPublisher publisher{outbox, meta};
  auto now = std::chrono::steady_clock::time_point{};
  constexpr std::array<std::chrono::milliseconds, 9> baseDelays = {
      250ms,   500ms,    1'000ms,  2'000ms, 4'000ms,
      8'000ms, 15'000ms, 30'000ms, 30'000ms};

  for (const auto base : baseDelays) {
    const auto result = publisher.step(now);
    if (result.code != PublisherStepCode::Retrying ||
        result.retryAfter < base || result.retryAfter > 30s ||
        result.retryAfter > base + base / 8) {
      return false;
    }
    if (publisher.step(now + result.retryAfter - 1ms).code !=
        PublisherStepCode::Waiting) {
      return false;
    }
    now += result.retryAfter;
  }

  return publisher.step(now).code == PublisherStepCode::Progress &&
         meta.publishCalls == 10u && outbox.retireCalls == 0u;
}

bool storageFailureIsRetryableRatherThanSilentIdle() {
  FakeOutbox outbox;
  outbox.unavailable = true;
  ScriptedMeta meta;
  SettlementPublisher publisher{outbox, meta};
  auto now = std::chrono::steady_clock::time_point{};
  const auto failed = publisher.step(now);
  if (failed.code != PublisherStepCode::Retrying ||
      failed.retryAfter != 250ms) {
    return false;
  }
  outbox.unavailable = false;
  now += failed.retryAfter;
  return publisher.step(now).code == PublisherStepCode::Progress &&
         meta.publishCalls == 1u;
}

bool responseLossReconcilesStatusBeforeRetirement() {
  FakeOutbox outbox;
  ScriptedMeta meta;
  meta.publishResults = {MetaPublishOutcome::ResponseLost,
                         MetaPublishOutcome::Applied};
  meta.statusResults = {MetaStatusOutcome::AcceptedPending,
                        MetaStatusOutcome::Applied};
  SettlementPublisher publisher{outbox, meta};
  auto now = std::chrono::steady_clock::time_point{};

  auto result = publisher.step(now);
  if (result.code != PublisherStepCode::Retrying) {
    return false;
  }
  now += result.retryAfter;
  result = publisher.step(now);
  if (result.code != PublisherStepCode::Retrying || meta.publishCalls != 1u ||
      meta.statusCalls != 1u) {
    return false;
  }
  now += result.retryAfter;
  if (publisher.step(now).code != PublisherStepCode::Progress ||
      outbox.retireCalls != 0u) {
    return false;
  }
  if (publisher.step(now).code != PublisherStepCode::Progress ||
      outbox.retireCalls != 0u) {
    return false;
  }
  return publisher.step(now).code == PublisherStepCode::Retired &&
         outbox.retireCalls == 1u && outbox.compactCalls == 1u;
}

bool responseLossBeforeAcceptRepostsAfterNotFound() {
  FakeOutbox outbox;
  ScriptedMeta meta;
  meta.publishResults = {MetaPublishOutcome::ResponseLost,
                         MetaPublishOutcome::Applied};
  meta.statusResults = {MetaStatusOutcome::NotFound};
  SettlementPublisher publisher{outbox, meta};
  auto now = std::chrono::steady_clock::time_point{};

  auto result = publisher.step(now);
  if (result.code != PublisherStepCode::Retrying) {
    return false;
  }
  now += result.retryAfter;
  result = publisher.step(now);
  if (result.code != PublisherStepCode::Retrying || meta.statusCalls != 1u) {
    return false;
  }
  now += result.retryAfter;
  return publisher.step(now).code == PublisherStepCode::Progress &&
         meta.publishCalls == 2u && outbox.retireCalls == 0u;
}

bool conflictNeverRetiresOrRetries() {
  FakeOutbox outbox;
  ScriptedMeta meta;
  meta.publishResults = {MetaPublishOutcome::Conflict};
  SettlementPublisher publisher{outbox, meta};
  const auto now = std::chrono::steady_clock::time_point{};
  return publisher.step(now).code == PublisherStepCode::Blocked &&
         publisher.step(now + 1h).code == PublisherStepCode::Blocked &&
         meta.publishCalls == 1u && outbox.retireCalls == 0u;
}

class DuplicateSafeMeta final : public SettlementMetaPort {
public:
  MetaPublishOutcome publish(const DurableSettlementIntent &intent) override {
    publishers_.arrive_and_wait();
    std::lock_guard lock{mutex_};
    ++publishCalls;
    const std::string key(intent.canonicalPayload.begin(),
                          intent.canonicalPayload.end());
    effects.emplace(key, 1u);
    return MetaPublishOutcome::Applied;
  }

  MetaStatusOutcome status(const DurableSettlementIntent &) override {
    return MetaStatusOutcome::Applied;
  }

  std::map<std::string, std::size_t> effects;
  std::size_t publishCalls{};

private:
  std::barrier<> publishers_{2};
  std::mutex mutex_;
};

bool duplicatePublishersProduceOneEffectPerIntent() {
  FakeOutbox outbox;
  DuplicateSafeMeta meta;
  SettlementPublisher first{outbox, meta};
  SettlementPublisher second{outbox, meta};
  const auto now = std::chrono::steady_clock::time_point{};
  std::thread firstThread{[&] {
    static_cast<void>(first.step(now));
    static_cast<void>(first.step(now));
    static_cast<void>(first.step(now));
  }};
  std::thread secondThread{[&] {
    static_cast<void>(second.step(now));
    static_cast<void>(second.step(now));
    static_cast<void>(second.step(now));
  }};
  firstThread.join();
  secondThread.join();
  return meta.effects.size() == 2u && meta.publishCalls == 4u &&
         outbox.retireCalls == 1u && outbox.retired;
}

bool metaClientUsesCanonicalContractAndClassifiesFailures() {
  const auto batch = durableBatch();
  std::vector<lol::meta::HttpsRequest> requests;
  std::vector<lol::meta::HttpsResult> results = {
      {.status = lol::meta::HttpsStatus::Response,
       .statusCode = 200,
       .body =
           R"({"settlementId":"d78f45fdc179931d82a4d1c390e84eae","status":"AcceptedPending"})"},
      {.status = lol::meta::HttpsStatus::Response,
       .statusCode = 200,
       .body =
           R"({"settlementId":"d78f45fdc179931d82a4d1c390e84eae","status":"Applied"})"},
      {.status = lol::meta::HttpsStatus::Timeout, .statusCode = 0, .body = {}},
      {.status = lol::meta::HttpsStatus::Response,
       .statusCode = 409,
       .body = R"({"code":"CONFLICT"})"},
      {.status = lol::meta::HttpsStatus::Response,
       .statusCode = 503,
       .body = R"({"code":"DEPENDENCY_UNAVAILABLE"})"},
      {.status = lol::meta::HttpsStatus::Response,
       .statusCode = 404,
       .body = R"({"code":"NOT_FOUND"})"},
  };
  lol::meta::MetaSettlementClient client{
      {.settlementsUrl = "https://meta.invalid/internal/v1/settlements",
       .serviceCredential = "fixture-service-credential",
       .timeout = 2s,
       .maxResponseBytes = 1024u},
      [&](lol::meta::HttpsRequest request, std::chrono::milliseconds) {
        requests.push_back(std::move(request));
        auto result = results.front();
        results.erase(results.begin());
        return result;
      }};

  const auto &intent = batch.intents.front();
  const auto accepted = client.publish(intent);
  const auto applied = client.status(intent);
  const auto lost = client.publish(intent);
  const auto conflict = client.publish(intent);
  const auto unavailable = client.publish(intent);
  const auto notFound = client.status(intent);
  return accepted == MetaPublishOutcome::AcceptedPending &&
         applied == MetaStatusOutcome::Applied &&
         lost == MetaPublishOutcome::ResponseLost &&
         conflict == MetaPublishOutcome::Conflict &&
         unavailable == MetaPublishOutcome::Retryable &&
         notFound == MetaStatusOutcome::NotFound && requests.size() == 6u &&
         requests[0].method == "POST" && requests[1].method == "GET" &&
         requests[0].authorization == "Bearer fixture-service-credential" &&
         requests[0].body.find("canonicalPayload") != std::string::npos &&
         requests[0].body.find("e29758929c2b28eeb609f9f3f5f321de183e9f0acd0ff91"
                               "ebe11d13e9ef51fd9") != std::string::npos;
}

bool retirementCompactionAndRestartKeepOnlyUnretiredBatches() {
  TempDirectory directory;
  const auto path = directory.path() / "segment-1.journal";
  SegmentJournal journal{path};
  if (!journal.recoverAndRepair().has_value()) {
    return false;
  }
  const auto first = journal.append(goldenBatch(0x62u));
  const auto second = journal.append(goldenBatch(0x72u));
  if (first.status != AppendResultStatus::DurablyQueued ||
      second.status != AppendResultStatus::DurablyQueued ||
      !first.commitSequence.has_value() || !second.commitSequence.has_value()) {
    return false;
  }
  const SettlementBatchId firstId{goldenBatch(0x62u).batchId};
  if (journal.retire(firstId, *first.commitSequence) !=
          OutboxRetireResult::Retired ||
      journal.retire(firstId, *first.commitSequence + 1u) !=
          OutboxRetireResult::NotFound) {
    return false;
  }
  const auto loadedBeforeCompaction = journal.nextUnretired();
  if (loadedBeforeCompaction.status != OutboxLoadStatus::Loaded ||
      !loadedBeforeCompaction.batch.has_value() ||
      loadedBeforeCompaction.batch->commitSequence != *second.commitSequence ||
      !journal.compact()) {
    return false;
  }

  SegmentJournal restarted{path};
  const auto recovered = restarted.recoverAndRepair();
  if (!recovered.has_value() ||
      recovered->status != JournalRecoveryStatus::Clean ||
      recovered->batches.size() != 1u || recovered->batches.front().retired ||
      recovered->batches.front().batchId != goldenBatch(0x72u).batchId ||
      recovered->batches.front().commitSequence != *second.commitSequence) {
    return false;
  }
  const SettlementBatchId secondId{goldenBatch(0x72u).batchId};
  if (restarted.retire(secondId, *first.commitSequence) !=
          OutboxRetireResult::NotFound ||
      restarted.nextUnretired().status != OutboxLoadStatus::Loaded) {
    return false;
  }
  return restarted.retire(firstId, *first.commitSequence) ==
             OutboxRetireResult::AlreadyRetired &&
         restarted.retire(secondId,
                          loadedBeforeCompaction.batch->commitSequence) ==
             OutboxRetireResult::Retired &&
         restarted.nextUnretired().status == OutboxLoadStatus::Empty;
}

} // namespace

int main() {
  return retriesWithFakeClockFrom250MillisecondsTo30Seconds() &&
                 storageFailureIsRetryableRatherThanSilentIdle() &&
                 responseLossReconcilesStatusBeforeRetirement() &&
                 responseLossBeforeAcceptRepostsAfterNotFound() &&
                 conflictNeverRetiresOrRetries() &&
                 duplicatePublishersProduceOneEffectPerIntent() &&
                 metaClientUsesCanonicalContractAndClassifiesFailures() &&
                 retirementCompactionAndRestartKeepOnlyUnretiredBatches()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
