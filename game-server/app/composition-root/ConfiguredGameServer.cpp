#include "ConfiguredGameServer.hpp"

#include "AuthClaimCoordinator.hpp"
#include "BattleLoadFlow.hpp"
#include "LobbyRoomFlow.hpp"
#include "RudpCombatFlow.hpp"
#include "RudpMovementFlow.hpp"
#include "SessionAuthFlow.hpp"

#include <lol/battle/BattleTime.hpp>
#include <lol/battle_continuity_storage/ContinuityStorage.hpp>
#include <lol/game_flow/BattleContinuityRecovery.hpp>
#include <lol/meta/CurlHttpsExchange.hpp>
#include <lol/meta/MetaClaimClient.hpp>
#include <lol/meta/MetaSettlementClient.hpp>
#include <lol/observability/GameMetrics.hpp>
#include <lol/observability/PrivateMetricsServer.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/ProcessLifecycle.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/runtime/linux/EpollReactor.hpp>
#include <lol/session/SessionRegistry.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>
#include <lol/settlement/SettlementIntent.hpp>
#include <lol/settlement/SettlementPublisher.hpp>
#include <lol/settlement_storage/JournalRecovery.hpp>
#include <lol/settlement_storage/SegmentJournal.hpp>
#include <lol/settlement_storage/StorageProbe.hpp>
#include <lol/settlement_storage/StorageWorker.hpp>
#include <lol/transport/rudp/RudpBindingRegistry.hpp>
#include <lol/transport/rudp/RudpCodec.hpp>
#include <lol/transport/rudp/RudpHeader.hpp>
#include <lol/transport/tcp/SessionProtocolCodec.hpp>
#include <lol/transport/tcp/TcpConnection.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lol::app {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMaximumConfigBytes = 64u * 1024u;
constexpr std::size_t kMaximumCredentialBytes = 4096u;
constexpr std::size_t kMaximumTcpFrameBytes = 64u * 1024u;
constexpr std::size_t kMaximumUdpDatagramBytes = 65535u;
constexpr std::size_t kMaximumDatagramsPerTurn = 64u;
constexpr std::size_t kApplicationQueueCapacity = 4096u;
constexpr auto kIngressPollInterval = 10ms;
constexpr auto kShutdownTimeout = 5s;
constexpr auto kMovementTickInterval =
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds{battle::BattleTime::tickNanos});
constexpr auto kRudpExpiryInterval = 250ms;
constexpr auto kBattleReconnectGrace = 30s;
constexpr std::uint64_t kJournalRecordOverhead = 60u;

volatile std::sig_atomic_t gStopSignal = 0;

extern "C" void requestProcessStop(int signalNumber) {
  gStopSignal = signalNumber;
}

struct ServerConfig final {
  std::string bindAddress;
  std::uint16_t tcpPort{};
  std::uint16_t udpPort{};
  std::filesystem::path journalPath;
  std::optional<std::filesystem::path> battleContinuityRoot;
  std::optional<std::filesystem::path> battleContinuityKeyFile;
  std::string metaClaimUrl;
  std::string metaSettlementsUrl;
  std::string metaServiceCredential;
  std::string metaCaCertificatePath;
  std::string metaCaCertificateSha256;
  std::string metaExpectedHostname;
  bool metricsEnabled{};
  std::string metricsBindAddress;
  std::uint16_t metricsPort{};
  std::string metricsReadCredential;
  std::string metricsSourceIdentityDigest;
  std::size_t metricsAllocatedCpuCount{};
  std::size_t workerThreads{};
  std::size_t workerQueueCapacity{};
  std::size_t deadlineCapacity{};
  std::size_t maxConnections{};
  bool testMetaFixture{};
};

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                            value.front() == '\r')) {
    value.remove_prefix(1u);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r')) {
    value.remove_suffix(1u);
  }
  return value;
}

bool containsControl(std::string_view value) {
  return std::ranges::any_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte < 0x20u || byte == 0x7fu;
  });
}

std::optional<std::string> readBoundedFile(const std::filesystem::path &path,
                                           std::size_t maximumBytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0u || size > maximumBytes) {
    return std::nullopt;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::nullopt;
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!input || input.peek() != std::char_traits<char>::eof()) {
    return std::nullopt;
  }
  return contents;
}

std::optional<std::string>
readPrivateCredential(const std::filesystem::path &path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return std::nullopt;
  }
  auto credential = readBoundedFile(path, kMaximumCredentialBytes);
  if (!credential.has_value() || credential->empty() ||
      containsControl(*credential)) {
    return std::nullopt;
  }
  return credential;
}

template <class Integer>
std::optional<Integer> parseUnsigned(std::string_view text, Integer maximum) {
  static_assert(std::is_unsigned_v<Integer>);
  if (text.empty()) {
    return std::nullopt;
  }
  Integer value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() ||
      value > maximum) {
    return std::nullopt;
  }
  return value;
}

std::optional<ServerConfig>
loadConfig(const std::filesystem::path &configPath) {
  auto contents = readBoundedFile(configPath, kMaximumConfigBytes);
  if (!contents.has_value()) {
    return std::nullopt;
  }
  std::map<std::string, std::string, std::less<>> values;
  std::size_t offset{};
  while (offset <= contents->size()) {
    const auto next = contents->find('\n', offset);
    const auto raw = std::string_view{*contents}.substr(
        offset, next == std::string::npos ? std::string::npos : next - offset);
    const auto line = trim(raw);
    if (!line.empty() && !line.starts_with('#')) {
      const auto separator = line.find('=');
      if (separator == std::string_view::npos) {
        return std::nullopt;
      }
      const auto key = trim(line.substr(0u, separator));
      const auto value = trim(line.substr(separator + 1u));
      if (key.empty() || value.empty() || containsControl(key) ||
          containsControl(value) ||
          !values.emplace(std::string{key}, std::string{value}).second) {
        return std::nullopt;
      }
    }
    if (next == std::string::npos) {
      break;
    }
    offset = next + 1u;
  }

  const std::set<std::string, std::less<>> required{
      "bind_address",
      "deadline_capacity",
      "journal_path",
      "max_connections",
      "metrics_enabled",
      "meta_claim_url",
      "meta_ca_certificate_file",
      "meta_ca_certificate_sha256",
      "meta_expected_hostname",
      "meta_service_credential_file",
      "meta_settlements_url",
      "tcp_port",
      "udp_port",
      "worker_queue_capacity",
      "worker_threads",
  };
  std::set<std::string, std::less<>> allowed = required;
  for (const auto *key :
       {"metrics_allocated_cpu_count", "metrics_bind_address", "metrics_port",
        "metrics_read_credential_file", "metrics_source_identity_digest",
        "battle_continuity_root", "battle_continuity_key_file"}) {
    allowed.insert(key);
  }
#if defined(LOOT_ENABLE_TEST_META_FIXTURE)
  allowed.insert("test_meta_fixture");
#endif
  if (values.size() < required.size() ||
      std::ranges::any_of(
          required,
          [&values](const auto &key) { return !values.contains(key); }) ||
      std::ranges::any_of(values, [&allowed](const auto &entry) {
        return !allowed.contains(entry.first);
      })) {
    return std::nullopt;
  }

  const auto tcpPort =
      parseUnsigned<std::uint16_t>(values.at("tcp_port"), UINT16_MAX);
  const auto udpPort =
      parseUnsigned<std::uint16_t>(values.at("udp_port"), UINT16_MAX);
  const auto workerThreads = parseUnsigned<std::size_t>(
      values.at("worker_threads"), std::size_t{256u});
  const auto workerQueue = parseUnsigned<std::size_t>(
      values.at("worker_queue_capacity"), std::size_t{65536u});
  const auto deadlineCapacity = parseUnsigned<std::size_t>(
      values.at("deadline_capacity"), std::size_t{65536u});
  const auto maxConnections = parseUnsigned<std::size_t>(
      values.at("max_connections"), std::size_t{65536u});
  const std::filesystem::path journalPath{values.at("journal_path")};
  const bool continuityEnabled = values.contains("battle_continuity_root");
  if (continuityEnabled != values.contains("battle_continuity_key_file")) {
    return std::nullopt;
  }
  const auto continuityRoot =
      continuityEnabled ? std::optional<std::filesystem::path>{values.at(
                              "battle_continuity_root")}
                        : std::nullopt;
  const auto continuityKeyFile =
      continuityEnabled ? std::optional<std::filesystem::path>{values.at(
                              "battle_continuity_key_file")}
                        : std::nullopt;
  auto credential = readPrivateCredential(
      std::filesystem::path{values.at("meta_service_credential_file")});
  if (values.at("metrics_enabled") != "true" &&
      values.at("metrics_enabled") != "false") {
    return std::nullopt;
  }
  const bool metricsEnabled = values.at("metrics_enabled") == "true";
  const std::array<std::string_view, 5> metricsKeys{
      "metrics_allocated_cpu_count", "metrics_bind_address", "metrics_port",
      "metrics_read_credential_file", "metrics_source_identity_digest"};
  if (std::ranges::any_of(metricsKeys, [&values, metricsEnabled](auto key) {
        return values.contains(key) != metricsEnabled;
      })) {
    return std::nullopt;
  }
  std::optional<std::uint16_t> metricsPort;
  std::optional<std::size_t> metricsAllocatedCpuCount;
  std::optional<std::string> metricsCredential;
  if (metricsEnabled) {
    metricsPort =
        parseUnsigned<std::uint16_t>(values.at("metrics_port"), UINT16_MAX);
    metricsAllocatedCpuCount = parseUnsigned<std::size_t>(
        values.at("metrics_allocated_cpu_count"), std::size_t{256u});
    metricsCredential = readPrivateCredential(
        std::filesystem::path{values.at("metrics_read_credential_file")});
  }
  if (!tcpPort.has_value() || !udpPort.has_value() ||
      !workerThreads.has_value() || *workerThreads == 0u ||
      !workerQueue.has_value() || *workerQueue == 0u ||
      !deadlineCapacity.has_value() || *deadlineCapacity == 0u ||
      !maxConnections.has_value() || *maxConnections == 0u ||
      !journalPath.is_absolute() || journalPath.filename().empty() ||
      (continuityEnabled &&
       (!continuityRoot->is_absolute() || continuityRoot->filename().empty() ||
        !continuityKeyFile->is_absolute() ||
        continuityKeyFile->filename().empty())) ||
      !credential.has_value() ||
      (metricsEnabled &&
       (!metricsPort.has_value() || *metricsPort == 0u ||
        !metricsAllocatedCpuCount.has_value() ||
        *metricsAllocatedCpuCount == 0u || !metricsCredential.has_value()))) {
    return std::nullopt;
  }

  in_addr parsedAddress{};
  if (::inet_pton(AF_INET, values.at("bind_address").c_str(), &parsedAddress) !=
      1) {
    return std::nullopt;
  }

  bool testMetaFixture = false;
#if defined(LOOT_ENABLE_TEST_META_FIXTURE)
  if (const auto fixture = values.find("test_meta_fixture");
      fixture != values.end()) {
    if (fixture->second != "true" && fixture->second != "false") {
      return std::nullopt;
    }
    testMetaFixture = fixture->second == "true";
  }
#endif

  return ServerConfig{
      .bindAddress = values.at("bind_address"),
      .tcpPort = *tcpPort,
      .udpPort = *udpPort,
      .journalPath = journalPath,
      .battleContinuityRoot = continuityRoot,
      .battleContinuityKeyFile = continuityKeyFile,
      .metaClaimUrl = values.at("meta_claim_url"),
      .metaSettlementsUrl = values.at("meta_settlements_url"),
      .metaServiceCredential = std::move(*credential),
      .metaCaCertificatePath = values.at("meta_ca_certificate_file"),
      .metaCaCertificateSha256 = values.at("meta_ca_certificate_sha256"),
      .metaExpectedHostname = values.at("meta_expected_hostname"),
      .metricsEnabled = metricsEnabled,
      .metricsBindAddress =
          metricsEnabled ? values.at("metrics_bind_address") : std::string{},
      .metricsPort = metricsEnabled ? *metricsPort : std::uint16_t{},
      .metricsReadCredential =
          metricsEnabled ? std::move(*metricsCredential) : std::string{},
      .metricsSourceIdentityDigest =
          metricsEnabled ? values.at("metrics_source_identity_digest")
                         : std::string{},
      .metricsAllocatedCpuCount =
          metricsEnabled ? *metricsAllocatedCpuCount : 0u,
      .workerThreads = *workerThreads,
      .workerQueueCapacity = *workerQueue,
      .deadlineCapacity = *deadlineCapacity,
      .maxConnections = *maxConnections,
      .testMetaFixture = testMetaFixture,
  };
}

template <class Value> class BoundedQueue final {
public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

  bool push(Value value) {
    std::lock_guard lock{mutex_};
    if (values_.size() >= capacity_) {
      return false;
    }
    values_.push_back(std::move(value));
    return true;
  }

  std::vector<Value> takeAll() {
    std::lock_guard lock{mutex_};
    std::vector<Value> result;
    result.reserve(values_.size());
    while (!values_.empty()) {
      result.push_back(std::move(values_.front()));
      values_.pop_front();
    }
    return result;
  }

private:
  const std::size_t capacity_;
  std::mutex mutex_;
  std::deque<Value> values_;
};

meta::HttpsResult unavailableExchange(meta::HttpsRequest,
                                      std::chrono::milliseconds) {
  return {
      .status = meta::HttpsStatus::NetworkFailure, .statusCode = 0, .body = {}};
}

#if defined(LOOT_ENABLE_TEST_META_FIXTURE)
std::optional<std::string> jsonString(std::string_view body,
                                      std::string_view field) {
  const std::string marker = "\"" + std::string{field} + "\":\"";
  const auto begin = body.find(marker);
  if (begin == std::string_view::npos) {
    return std::nullopt;
  }
  const auto valueBegin = begin + marker.size();
  const auto end = body.find('"', valueBegin);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  return std::string{body.substr(valueBegin, end - valueBegin)};
}

meta::HttpsResult fixtureExchange(meta::HttpsRequest request,
                                  std::chrono::milliseconds) {
  if (request.url.ends_with("/internal/v1/game-credentials/claim")) {
    const auto credential = jsonString(request.body, "credential");
    if (!credential.has_value() || credential->size() != 43u) {
      return {.status = meta::HttpsStatus::Response,
              .statusCode = 400,
              .body = R"({"code":"INVALID"})"};
    }
    if (credential->front() < 'A' || credential->front() > 'J') {
      return {.status = meta::HttpsStatus::Response,
              .statusCode = 400,
              .body = R"({"code":"INVALID"})"};
    }
    const unsigned suffix =
        static_cast<unsigned>(credential->front() - 'A') + 1u;
    const std::string suffixText =
        suffix < 10u ? "0" + std::to_string(suffix) : std::to_string(suffix);
    return {
        .status = meta::HttpsStatus::Response,
        .statusCode = 200,
        .body = "{\"accountId\":\"00000000-0000-4000-8000-0000000000" +
                suffixText + "\",\"nickname\":\"player-" +
                std::to_string(suffix) + "\"}",
    };
  }
  if (request.url.find("/internal/v1/settlements") != std::string::npos) {
    auto settlementId = jsonString(request.body, "settlementId");
    if (!settlementId.has_value() && request.method == "GET") {
      const auto separator = request.url.rfind('/');
      if (separator != std::string::npos) {
        settlementId = request.url.substr(separator + 1u);
      }
    }
    if (settlementId.has_value()) {
      return {.status = meta::HttpsStatus::Response,
              .statusCode = 200,
              .body = "{\"settlementId\":\"" + *settlementId +
                      "\",\"status\":\"Applied\"}"};
    }
  }
  return unavailableExchange(std::move(request), 0ms);
}
#endif

meta::HttpsExchange exchangeFor(const ServerConfig &config) {
#if defined(LOOT_ENABLE_TEST_META_FIXTURE)
  if (config.testMetaFixture) {
    return fixtureExchange;
  }
#endif
  return meta::makeCurlHttpsExchange(meta::CurlHttpsExchangeConfig{
      .caCertificatePath = config.metaCaCertificatePath,
      .caCertificateSha256 = config.metaCaCertificateSha256,
      .expectedHostname = config.metaExpectedHostname,
      .connectTimeout = 500ms,
      .totalTimeout = 2s,
      .maxRequestBytes = 64u * 1024u,
      .maxResponseBytes = 64u * 1024u,
  });
}

class ObservedSettlementStorage final
    : public settlement::SettlementStoragePort,
      public settlement::SettlementOutboxPort {
public:
  ObservedSettlementStorage(
      settlement_storage::StorageWorker &storage,
      settlement::SettlementCapacityGate &capacity,
      const settlement_storage::JournalRecoveryResult &recovery,
      observability::GameMetrics *gameMetrics)
      : storage_(storage), capacity_(capacity), gameMetrics_(gameMetrics) {
    const auto recoveredAt = std::chrono::steady_clock::now() - 15min;
    for (const auto &batch : recovery.batches) {
      if (batch.retired) {
        continue;
      }
      const auto metrics = metricsFor(batch.canonicalIntents, recoveredAt);
      entries_.emplace(batch.batchId, metrics);
      records_ += metrics.records;
      bytes_ += metrics.bytes;
    }
    publishSnapshotLocked();
  }

  settlement::SubmitAppendResult
  submit(settlement::DurableAppendRequest request,
         CompletionSink completion) override {
    if (!completion) {
      return settlement::SubmitAppendResult::StorageUnavailable;
    }
    const auto key = request.batchId.bytes();
    const auto entryMetrics =
        metricsFor(request.canonicalIntents, std::chrono::steady_clock::now());
    {
      std::lock_guard lock{mutex_};
      ++pendingAppends_;
    }
    const auto submitted = storage_.submit(
        std::move(request),
        [this, key, entryMetrics, completion = std::move(completion)](
            settlement::DurableAppendOutcome outcome) mutable {
          {
            std::lock_guard lock{mutex_};
            --pendingAppends_;
            if (std::holds_alternative<settlement::DurableAppendCompleted>(
                    outcome)) {
              const auto [entry, inserted] =
                  entries_.emplace(key, entryMetrics);
              if (inserted) {
                records_ += entry->second.records;
                bytes_ += entry->second.bytes;
              }
              if (gameMetrics_ != nullptr) {
                gameMetrics_->recordDurableAppend(
                    std::chrono::steady_clock::now() - entryMetrics.createdAt);
              }
            } else {
              healthy_ = false;
            }
            publishSnapshotLocked();
          }
          idle_.notify_all();
          completion(std::move(outcome));
        });
    if (submitted != settlement::SubmitAppendResult::Accepted) {
      std::lock_guard lock{mutex_};
      --pendingAppends_;
      if (submitted == settlement::SubmitAppendResult::StorageUnavailable) {
        healthy_ = false;
      }
      publishSnapshotLocked();
      idle_.notify_all();
    }
    return submitted;
  }

  settlement::OutboxLoadResult nextUnretired() override {
    auto result = storage_.nextUnretired();
    if (result.status == settlement::OutboxLoadStatus::Unavailable) {
      std::lock_guard lock{mutex_};
      healthy_ = false;
      publishSnapshotLocked();
    }
    return result;
  }

  settlement::OutboxRetireResult
  retire(const settlement::SettlementBatchId &batchId,
         std::uint64_t commitSequence) override {
    const auto result = storage_.retire(batchId, commitSequence);
    std::lock_guard lock{mutex_};
    if (result == settlement::OutboxRetireResult::Retired ||
        result == settlement::OutboxRetireResult::AlreadyRetired) {
      const auto entry = entries_.find(batchId.bytes());
      if (entry != entries_.end()) {
        records_ -= entry->second.records;
        bytes_ -= entry->second.bytes;
        entries_.erase(entry);
      }
    } else if (result == settlement::OutboxRetireResult::Unavailable) {
      healthy_ = false;
    }
    publishSnapshotLocked();
    return result;
  }

  bool compact() override {
    const bool compacted = storage_.compact();
    if (!compacted) {
      std::lock_guard lock{mutex_};
      healthy_ = false;
      publishSnapshotLocked();
    }
    return compacted;
  }

  void refresh() {
    std::lock_guard lock{mutex_};
    publishSnapshotLocked();
  }

  bool waitUntilIdle(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return idle_.wait_for(lock, timeout,
                          [this] { return pendingAppends_ == 0u; });
  }

private:
  struct Metrics final {
    std::uint64_t records{};
    std::uint64_t bytes{};
    std::chrono::steady_clock::time_point createdAt;
  };

  static Metrics
  metricsFor(const std::vector<std::vector<std::uint8_t>> &intents,
             std::chrono::steady_clock::time_point createdAt) {
    std::uint64_t bytes = kJournalRecordOverhead + 18u +
                          static_cast<std::uint64_t>(intents.size()) * 40u;
    for (const auto &intent : intents) {
      bytes +=
          kJournalRecordOverhead + static_cast<std::uint64_t>(intent.size());
    }
    return {.records = static_cast<std::uint64_t>(intents.size()) + 1u,
            .bytes = bytes,
            .createdAt = createdAt};
  }

  void publishSnapshotLocked() {
    std::chrono::milliseconds oldest{};
    if (!entries_.empty()) {
      const auto oldestEntry =
          std::ranges::min_element(entries_, {}, [](const auto &entry) {
            return entry.second.createdAt;
          });
      oldest = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - oldestEntry->second.createdAt);
    }
    capacity_.updateBacklog(settlement::OutboxBacklogSnapshot{
        .unretiredRecords = records_,
        .unretiredBytes = bytes_,
        .oldestPendingAge = oldest,
        .storageHealthy = healthy_,
    });
  }

  settlement_storage::StorageWorker &storage_;
  settlement::SettlementCapacityGate &capacity_;
  observability::GameMetrics *gameMetrics_;
  std::mutex mutex_;
  std::condition_variable idle_;
  std::map<std::array<std::uint8_t, 16>, Metrics> entries_;
  std::uint64_t records_{};
  std::uint64_t bytes_{};
  std::size_t pendingAppends_{};
  bool healthy_{true};
};

class SettlementPublisherDriver final {
public:
  explicit SettlementPublisherDriver(settlement::SettlementPublisher &publisher)
      : publisher_(publisher) {}

  ~SettlementPublisherDriver() { stop(); }

  bool start() {
    if (thread_.joinable()) {
      return false;
    }
    try {
      thread_ = std::thread{[this] { run(); }};
    } catch (...) {
      return false;
    }
    return true;
  }

  void stop() {
    {
      std::lock_guard lock{mutex_};
      stopping_ = true;
    }
    changed_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  void run() {
    while (true) {
      {
        std::lock_guard lock{mutex_};
        if (stopping_) {
          return;
        }
      }
      const auto result = publisher_.step(std::chrono::steady_clock::now());
      auto delay = 50ms;
      if (result.code == settlement::PublisherStepCode::Waiting ||
          result.code == settlement::PublisherStepCode::Retrying) {
        delay = std::clamp(result.retryAfter, 1ms, 250ms);
      } else if (result.code == settlement::PublisherStepCode::Blocked) {
        delay = 250ms;
      }
      std::unique_lock lock{mutex_};
      changed_.wait_for(lock, delay, [this] { return stopping_; });
    }
  }

  settlement::SettlementPublisher &publisher_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread thread_;
  bool stopping_{};
};

class FileDescriptor final {
public:
  FileDescriptor() = default;
  explicit FileDescriptor(int value) : value_(value) {}
  ~FileDescriptor() { reset(); }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }
  int release() noexcept { return std::exchange(value_, -1); }
  void reset(int value = -1) noexcept {
    if (value_ >= 0) {
      ::close(value_);
    }
    value_ = value;
  }

private:
  int value_{-1};
};

bool setNonBlockingCloseOnExec(int descriptor) {
  const int statusFlags = ::fcntl(descriptor, F_GETFL, 0);
  const int descriptorFlags = ::fcntl(descriptor, F_GETFD, 0);
  return statusFlags >= 0 && descriptorFlags >= 0 &&
         ::fcntl(descriptor, F_SETFL, statusFlags | O_NONBLOCK) == 0 &&
         ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0;
}

std::optional<FileDescriptor> bindSocket(int type, std::string_view bindAddress,
                                         std::uint16_t port,
                                         std::uint16_t &boundPort) {
  FileDescriptor descriptor{::socket(AF_INET, type, 0)};
  if (!descriptor.valid() || !setNonBlockingCloseOnExec(descriptor.get())) {
    return std::nullopt;
  }
  const int reuse = 1;
  if (::setsockopt(descriptor.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) != 0) {
    return std::nullopt;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, std::string{bindAddress}.c_str(),
                  &address.sin_addr) != 1 ||
      ::bind(descriptor.get(), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    return std::nullopt;
  }
  socklen_t addressBytes = sizeof(address);
  if (::getsockname(descriptor.get(), reinterpret_cast<sockaddr *>(&address),
                    &addressBytes) != 0) {
    return std::nullopt;
  }
  boundPort = ntohs(address.sin_port);
  return descriptor;
}

transport::rudp::RudpEndpoint endpointFor(const sockaddr_in &address) {
  transport::rudp::RudpEndpoint endpoint{
      .address = {}, .port = ntohs(address.sin_port), .scopeId = 0u};
  endpoint.address[10] = std::byte{0xff};
  endpoint.address[11] = std::byte{0xff};
  std::memcpy(endpoint.address.data() + 12u, &address.sin_addr,
              sizeof(address.sin_addr));
  return endpoint;
}

std::optional<sockaddr_in>
socketAddressFor(const transport::rudp::RudpEndpoint &endpoint) {
  if (endpoint.port == 0u || endpoint.scopeId != 0u ||
      endpoint.address[10] != std::byte{0xff} ||
      endpoint.address[11] != std::byte{0xff} ||
      std::ranges::any_of(
          std::span{endpoint.address}.first<10>(),
          [](std::byte value) { return value != std::byte{0}; })) {
    return std::nullopt;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);
  std::memcpy(&address.sin_addr, endpoint.address.data() + 12u,
              sizeof(address.sin_addr));
  return address;
}

std::uint64_t unixTimeMilliseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

shared::SessionGeneration firstSessionGeneration(
    const battle_continuity_storage::ContinuityStorage *storage) noexcept {
  if (storage == nullptr) {
    return shared::SessionGeneration{1U};
  }
  return shared::SessionGeneration{
      (static_cast<std::uint64_t>(storage->writerRecoveryEpoch()) << 32U) |
      1ULL};
}

class ConfiguredGameServer final {
public:
  ConfiguredGameServer(
      ServerConfig config, settlement_storage::JournalRecoveryResult recovery,
      std::unique_ptr<battle_continuity_storage::ContinuityStorage>
          continuityStorage,
      battle_continuity_storage::ScanResult continuityScan)
      : config_(std::move(config)), recovery_(std::move(recovery)),
        continuityStorage_(std::move(continuityStorage)),
        continuityScan_(std::move(continuityScan)),
        claimCompletions_(kApplicationQueueCapacity),
        roomOutbounds_(kApplicationQueueCapacity),
        movementSnapshots_(kApplicationQueueCapacity),
        combatOutbounds_(kApplicationQueueCapacity),
        movementDatagrams_(kApplicationQueueCapacity),
        sessions_(firstSessionGeneration(continuityStorage_.get())) {}

  ~ConfiguredGameServer() { static_cast<void>(stop()); }

  bool start() {
    if (started_) {
      return false;
    }
    const auto firstRoomId =
        continuityStorage_ != nullptr
            ? std::optional{shared::RoomId{
                  (static_cast<std::uint64_t>(
                       continuityStorage_->writerRecoveryEpoch())
                   << 32U) |
                  1ULL}}
            : game_flow::RoomCommandGateway::firstRoomIdForSettlementHistory(
                  recovery_.lastSequence);
    if (!firstRoomId.has_value()) {
      return false;
    }
    try {
      workers_ =
          std::make_unique<runtime::WorkerPool>(runtime::WorkerPoolConfig{
              .threadCount = config_.workerThreads,
              .queueCapacity = config_.workerQueueCapacity,
          });
      deadlines_ = std::make_unique<runtime::ThreadDeadlineScheduler>(
          runtime::ThreadDeadlineSchedulerConfig{.queueCapacity =
                                                     config_.deadlineCapacity});
      storageWorker_ = std::make_unique<settlement_storage::StorageWorker>(
          config_.journalPath);
      if (config_.metricsEnabled) {
        gameMetrics_ = std::make_unique<observability::GameMetrics>(
            config_.metricsAllocatedCpuCount);
      }
      observedStorage_ = std::make_unique<ObservedSettlementStorage>(
          *storageWorker_, capacity_, recovery_, gameMetrics_.get());
      readiness_ = std::make_unique<RudpGameplayReadiness>(bindings_);
      correlations_ =
          std::make_unique<AuthClaimCoordinator>(sessions_, bindings_);
      const auto exchange = exchangeFor(config_);
      claimClient_ = std::make_unique<meta::MetaClaimClient>(
          meta::MetaClaimClientConfig{
              .claimUrl = config_.metaClaimUrl,
              .serviceCredential = config_.metaServiceCredential,
              .timeout = 500ms,
              .maxOutstanding = config_.maxConnections,
              .maxResponseBytes = 4096u,
          },
          exchange, [this](meta::ClaimCompletion completion) {
            if (!claimCompletions_.push(std::move(completion))) {
              fatalStop_.store(true, std::memory_order_release);
            }
          });
      authFlow_ =
          std::make_unique<SessionAuthFlow>(*correlations_, *claimClient_);
      auto roomOutbound = [this](game_flow::LobbyRoomOutboundIntent intent) {
        if (!roomOutbounds_.push(std::move(intent))) {
          fatalStop_.store(true, std::memory_order_release);
        }
      };
      auto movementSnapshot = [this](battle::StateSnapshotProjection snapshot) {
        if (!movementSnapshots_.push(std::move(snapshot))) {
          fatalStop_.store(true, std::memory_order_release);
        }
      };
      auto combatOutbound = [this](game_flow::CombatOutboundIntent intent) {
        if (!combatOutbounds_.push(std::move(intent))) {
          fatalStop_.store(true, std::memory_order_release);
        }
      };
      if (continuityStorage_ != nullptr) {
        gateway_ = std::make_unique<game_flow::RoomCommandGateway>(
            *workers_, *readiness_, std::move(roomOutbound),
            std::move(movementSnapshot), std::move(combatOutbound), *deadlines_,
            capacity_, *observedStorage_, *continuityStorage_, *firstRoomId);
      } else {
        gateway_ = std::make_unique<game_flow::RoomCommandGateway>(
            *workers_, *readiness_, std::move(roomOutbound),
            std::move(movementSnapshot), std::move(combatOutbound), *deadlines_,
            capacity_, *observedStorage_, *firstRoomId);
      }
      movementFlow_ = std::make_unique<RudpMovementFlow>(bindings_, *gateway_);
      combatFlow_ = std::make_unique<RudpCombatFlow>(bindings_, *gateway_);
      lobbyFlow_ = std::make_unique<LobbyRoomFlow>(*gateway_);
      battleLoadFlow_ = std::make_unique<BattleLoadFlow>(*gateway_);
      settlementClient_ = std::make_unique<meta::MetaSettlementClient>(
          meta::MetaSettlementClientConfig{
              .settlementsUrl = config_.metaSettlementsUrl,
              .serviceCredential = config_.metaServiceCredential,
              .timeout = 500ms,
              .maxResponseBytes = 4096u,
          },
          exchange);
      publisher_ = std::make_unique<settlement::SettlementPublisher>(
          *observedStorage_, *settlementClient_);
      publisherDriver_ =
          std::make_unique<SettlementPublisherDriver>(*publisher_);
      if (continuityStorage_ != nullptr) {
        recovering_ = true;
        std::vector<game_flow::StoredBattleRecovery> battles;
        battles.reserve(continuityScan_.healthy.size());
        for (auto &stored : continuityScan_.healthy) {
          battles.push_back(game_flow::StoredBattleRecovery{
              .identity = stored.identity,
              .committedJournal = std::move(stored.committedJournal),
              .privateEnvelope = std::move(stored.privateEnvelope),
          });
        }

        auto dispose = [this](game_flow::BattleRecoveryRequest request) {
          const auto epoch = continuityStorage_->writerRecoveryEpoch();
          const auto error =
              request.disposition ==
                      game_flow::BattleRecoveryDisposition::Retire
                  ? continuityStorage_->retireBattle(request.stored.identity,
                                                     epoch)
                  : continuityStorage_->quarantineBattle(
                        request.stored.identity, epoch);
          if (error != battle_continuity_storage::StorageError::None) {
            return false;
          }
          std::cerr << "battle recovery disposition room="
                    << request.stored.identity.roomId.value() << " battle="
                    << request.stored.identity.battleInstanceId.value()
                    << " disposition="
                    << (request.disposition ==
                                game_flow::BattleRecoveryDisposition::Retire
                            ? "retire"
                            : "quarantine");
          if (request.reason.has_value()) {
            std::cerr << " reason="
                      << static_cast<unsigned int>(*request.reason);
          }
          std::cerr << '\n';
          return true;
        };
        auto settlementDurable =
            [this](const settlement::SettlementIntentBatch &batch) {
              std::vector<std::vector<std::uint8_t>> canonicalIntents;
              canonicalIntents.reserve(batch.intents().size());
              for (const auto &intent : batch.intents()) {
                canonicalIntents.push_back(settlement::canonicalPayload(intent));
              }
              return std::ranges::any_of(
                  recovery_.batches, [&](const auto &candidate) {
                    return candidate.batchId == batch.id().bytes() &&
                           candidate.canonicalIntents == canonicalIntents;
                  });
            };
        auto install = [this](game_flow::StartupRecoveredBattleInstall recovered) {
          if (!recovered.reconstruction.room.has_value() ||
              !recovered.reconstruction.roomRecoveryState.has_value()) {
            return false;
          }
          auto seed = makeRecoveredRuntimeSeed(recovered.reconstruction);
          if (!seed.has_value()) {
            return false;
          }
          if (!gateway_->installRecoveredBattle(
                  game_flow::RecoveredBattleInstall{
                      .room = std::move(*recovered.reconstruction.room),
                      .battle = std::move(recovered.reconstruction.battle),
                      .recording =
                          std::move(recovered.reconstruction.recording),
                      .nextBattleOrdinal = recovered.reconstruction
                                               .roomRecoveryState
                                               ->nextBattleOrdinal,
                      .settlementBatch =
                          std::move(recovered.reconstruction.settlementBatch),
                      .settlementAlreadyDurable =
                          recovered.settlementAlreadyDurable,
                  })) {
            return false;
          }
          applyRecoveredRuntimeSeed(std::move(*seed),
                                    recovered.reconnectExpiresAt);
          return true;
        };
        if (!game_flow::recoverBattlesAtStartup(
                game_flow::StartupRecoveryInput{
                    .battles = std::move(battles),
                    .currentWriterRecoveryEpoch =
                        continuityStorage_->writerRecoveryEpoch(),
                    .reconnectExpiresAt =
                        std::chrono::steady_clock::now() +
                        kBattleReconnectGrace,
                    .sessions = sessions_,
                    .settlementStorage = *observedStorage_,
                    .dispose = std::move(dispose),
                    .settlementDurable = std::move(settlementDurable),
                    .install = std::move(install),
                })) {
          std::cerr << "startup failed: battle recovery\n";
          return false;
        }
        if (!workers_->waitUntilIdle(kShutdownTimeout) ||
            !observedStorage_->waitUntilIdle(kShutdownTimeout) ||
            !workers_->waitUntilIdle(kShutdownTimeout)) {
          std::cerr << "startup failed: battle recovery drain\n";
          return false;
        }
        processRoomOutbounds();
        processCombatOutbounds();
        if (fatalStop_.load(std::memory_order_acquire) || recoveryFailed_) {
          std::cerr << "startup failed: battle recovery outcome\n";
          return false;
        }
        continuityScan_.healthy.clear();
      }
      if (config_.metricsEnabled) {
        metricsServer_ = std::make_unique<observability::PrivateMetricsServer>(
            observability::PrivateMetricsServerConfig{
                .bindAddress = config_.metricsBindAddress,
                .port = config_.metricsPort,
                .readToken = config_.metricsReadCredential,
                .sourceIdentityDigest = config_.metricsSourceIdentityDigest,
                .requestTimeout = 1s,
                .maxRequestBytes = 64u * 1024u,
                .maxResponseBytes = 64u * 1024u,
            },
            [this] {
              auto snapshot = gameMetrics_->snapshot(gateway_->observation(),
                                                     capacity_.metrics());
              const auto reliable = combatFlow_->reliabilityObservation();
              snapshot.rudpAcceptedSamples =
                  static_cast<double>(reliable.acceptedSamples);
              snapshot.rudpRetransmittedSamples =
                  static_cast<double>(reliable.retransmittedSamples);
              snapshot.rudpUnconfirmedSamples =
                  static_cast<double>(reliable.unconfirmedSamples);
              snapshot.rudpNonpositiveSamples =
                  static_cast<double>(reliable.nonpositiveSamples);
              snapshot.rudpExpiredSamples =
                  static_cast<double>(reliable.expiredSamples);
              snapshot.rudpStaleSamples =
                  static_cast<double>(reliable.staleSamples);
              snapshot.rudpCoalescedSamples =
                  static_cast<double>(reliable.coalescedSamples);
              snapshot.rudpNoNewEntryAcks =
                  static_cast<double>(reliable.noNewEntryAcks);
              snapshot.rudpRetransmissions =
                  static_cast<double>(reliable.retransmissions);
              snapshot.rudpExpiries = static_cast<double>(reliable.expiries);
              snapshot.rudpSendFailures =
                  static_cast<double>(reliable.sendFailures);
              for (const auto interval : reliable.effectiveRtos) {
                snapshot.rudpEffectiveRtoMs.push_back(
                    static_cast<double>(interval.count()));
              }
              const auto recovery = bindings_.recoveryObservation();
              snapshot.rudpRecoveryEntered =
                  static_cast<double>(recovery.entered);
              snapshot.rudpRecoveryEscalated =
                  static_cast<double>(recovery.escalated);
              snapshot.rudpRecoveryReset = static_cast<double>(recovery.reset);
              snapshot.rudpRecoveryStaleTimeouts =
                  static_cast<double>(recovery.staleTimeouts);
              snapshot.rudpRecoveryActive = 0;
              for (const auto &policy : recovery.policies) {
                snapshot.rudpInitialRtoMs.push_back(
                    static_cast<double>(policy.initialRto.count()));
                if (policy.recoveryFloor.count() > 0) {
                  ++*snapshot.rudpRecoveryActive;
                  snapshot.rudpRecoveryFloorMs.push_back(
                      static_cast<double>(policy.recoveryFloor.count()));
                }
              }
              for (const auto &estimate : bindings_.rttSnapshots()) {
                snapshot.rudpBaseRtoMs.push_back(
                    static_cast<double>(estimate.rto.count()));
                if (estimate.samples != 0) {
                  snapshot.rudpSrttMs.push_back(
                      std::chrono::duration<double, std::milli>{estimate.srtt}
                          .count());
                  snapshot.rudpRttvarMs.push_back(
                      std::chrono::duration<double, std::milli>{estimate.rttvar}
                          .count());
                }
              }
              return snapshot;
            });
      }

      if (runtime::linux::EpollReactor::supported()) {
        reactor_ = std::make_unique<runtime::linux::EpollReactor>(
            config_.maxConnections + 2u);
        if (!reactor_->valid()) {
          std::cerr << "startup failed: reactor\n";
          return false;
        }
      }

      auto tcp = bindSocket(SOCK_STREAM, config_.bindAddress, config_.tcpPort,
                            tcpPort_);
      const int tcpBindError = tcp.has_value() ? 0 : errno;
      auto udp = bindSocket(SOCK_DGRAM, config_.bindAddress, config_.udpPort,
                            udpPort_);
      const int udpBindError = udp.has_value() ? 0 : errno;
      if (!tcp.has_value() || !udp.has_value()) {
        std::cerr << "startup failed: bind tcp_errno=" << tcpBindError
                  << " udp_errno=" << udpBindError << '\n';
        return false;
      }
      tcpListener_ = std::move(*tcp);
      udpSocket_ = std::move(*udp);
      if (reactor_ != nullptr && (!reactor_->watch(tcpListener_.get()) ||
                                  !reactor_->watch(udpSocket_.get()))) {
        std::cerr << "startup failed: reactor watch\n";
        return false;
      }
      if ((metricsServer_ != nullptr && !metricsServer_->start()) ||
          !publisherDriver_->start() || !lifecycle_.start()) {
        std::cerr << "startup failed: lifecycle\n";
        return false;
      }
      if (::listen(tcpListener_.get(), 128) != 0) {
        std::cerr << "startup failed: listen\n";
        return false;
      }
      if (continuityStorage_ != nullptr) {
        if (!gateway_->activateRecoveredBattles()) {
          std::cerr << "startup failed: recovered activation\n";
          return false;
        }
        recovering_ = false;
      }
      nextMovementTick_ =
          std::chrono::steady_clock::now() + kMovementTickInterval;
      nextRudpExpiry_ = std::chrono::steady_clock::now() + kRudpExpiryInterval;
      started_ = true;
      std::cout << "READY tcp=" << tcpPort_ << " udp=" << udpPort_ << '\n'
                << std::flush;
      return true;
    } catch (const std::exception &error) {
      std::cerr << "startup failed: exception: " << error.what() << '\n';
      return false;
    } catch (...) {
      std::cerr << "startup failed: unknown exception\n";
      return false;
    }
  }

  int run() {
    if (!started_) {
      return 1;
    }
    while (gStopSignal == 0 && !fatalStop_.load(std::memory_order_acquire)) {
      processClaimCompletions();
      processRoomOutbounds();
      processMovementSnapshots();
      processCombatOutbounds();
      processUdpOutbounds();
      processPeriodicWork();
      waitForIngress();
      closeDrainedConnections();
    }
    const bool gracefulSignal = gStopSignal == SIGINT || gStopSignal == SIGTERM;
    return stop() && gracefulSignal &&
                   !fatalStop_.load(std::memory_order_acquire)
               ? 0
               : 1;
  }

private:
  struct ConnectionState final {
    ConnectionState(std::uint64_t connectionEpoch,
                    std::chrono::steady_clock::time_point openedAt)
        : epoch(connectionEpoch),
          connection(
              transport::tcp::PreAuthLimits{
                  .maxFrameBytes = kMaximumTcpFrameBytes,
                  .maxPreAuthBytes = kMaximumTcpFrameBytes + 4u,
                  .maxPreAuthReadEvents = 32u,
                  .preAuthTimeout = 5s,
                  .maxOutboundBytes = 1024u * 1024u,
              },
              openedAt) {}

    std::uint64_t epoch;
    transport::tcp::TcpConnection connection;
    std::optional<game_flow::AuthenticatedRoomSession> session;
    std::optional<std::uint64_t> resumeRequestId;
    std::optional<std::uint64_t> pendingResumeSnapshotId;
    std::vector<std::byte> postAuthInbound;
  };

  struct ActiveBattleTick final {
    shared::BattleInstanceId battleId;
    std::uint32_t nextServerTick{1u};
    transport::tcp::BattleResumePhase phase{
        transport::tcp::BattleResumePhase::Combat};
    std::chrono::steady_clock::time_point deadline;
  };

  struct DetachedBattleSession final {
    game_flow::AuthenticatedRoomSession session;
    shared::RoomId roomId;
    shared::BattleInstanceId battleId;
    std::chrono::steady_clock::time_point expiresAt;
    bool resuming{false};
  };

  struct PendingResume final {
    std::uint64_t connectionEpoch;
    std::uint64_t requestId;
  };

  struct BattleStateCache final {
    shared::RoomId roomId{0u};
    shared::BattleInstanceId battleId{0u};
    std::optional<battle_continuity::BattleIdentity> continuityIdentity;
    std::vector<game_flow::BattleParticipantProjection> participants;
    std::optional<battle::StateSnapshotProjection> movement;
    std::optional<battle::CombatProjection> combat;
    std::optional<battle::LootProjection> loot;
    std::optional<battle::BattleFinalResult> finalResult;
  };

  struct RecoveredRuntimeSeed final {
    shared::RoomId roomId;
    shared::BattleInstanceId battleId;
    battle_continuity::BattleIdentity continuityIdentity;
    std::vector<game_flow::AuthenticatedRoomSession> activeSessions;
    std::vector<game_flow::BattleParticipantProjection> participants;
    battle::StateSnapshotProjection movement;
    std::optional<battle::CombatProjection> combat;
    battle::LootProjection loot;
    std::optional<battle::BattleFinalResult> finalResult;
    std::optional<ActiveBattleTick> activeTick;
  };

  using BattleKey = std::pair<shared::RoomId, shared::BattleInstanceId>;

  std::optional<RecoveredRuntimeSeed> makeRecoveredRuntimeSeed(
      const game_flow::RecoveredBattleReconstruction &recovered) const {
    const auto load = recovered.battle.projection();
    const auto state = recovered.battle.exportDeterministicState();
    const auto movement = recovered.battle.movementProjection();
    const auto result = recovered.battle.resultProjection().result;
    if (recovered.recording.records().empty()) {
      return std::nullopt;
    }
    const auto &first = recovered.recording.records().front().header;
    const battle_continuity::BattleIdentity continuityIdentity{
        .originRecoveryEpoch = first.originRecoveryEpoch,
        .roomId = first.roomId,
        .battleInstanceId = first.battleInstanceId};
    if (continuityIdentity.roomId != load.roomId ||
        continuityIdentity.battleInstanceId != load.battleId) {
      return std::nullopt;
    }

    std::vector<game_flow::AuthenticatedRoomSession> activeSessions;
    activeSessions.reserve(recovered.activeParticipants.size());
    for (const auto &participant : recovered.activeParticipants) {
      activeSessions.push_back(game_flow::AuthenticatedRoomSession{
          .accountId = participant.accountId,
          .sessionId = participant.sessionId,
          .generation = participant.generation,
          .nickname = participant.nickname,
      });
    }

    std::vector<game_flow::BattleParticipantProjection> participants;
    if (!load.capturedParticipants.empty()) {
      participants.reserve(load.capturedParticipants.size());
      for (const auto &participant : load.capturedParticipants) {
        participants.push_back(game_flow::BattleParticipantProjection{
            .sessionId = participant.sessionId,
            .generation = participant.generation,
            .nickname = participant.nickname,
        });
      }
    } else {
      participants.reserve(recovered.activeParticipants.size());
      for (const auto &identity : recovered.activeParticipants) {
        participants.push_back(game_flow::BattleParticipantProjection{
            .sessionId = identity.sessionId,
            .generation = identity.generation,
            .nickname = identity.nickname,
        });
      }
    }

    std::optional<ActiveBattleTick> activeTick;
    if (state.state == battle::BattleLoadState::GameplayCommitted &&
        state.resultState != battle::BattleResultState::Committed) {
      if (movement.serverTick == std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
      }
      const bool lootPhase =
          state.lootResolution != battle::LootResolutionState::NotStarted;
      const auto deadlineTick =
          lootPhase ? state.lootDeadlineTick : state.combatDeadlineTick;
      if (!deadlineTick.has_value()) {
        return std::nullopt;
      }
      const auto remainingTicks =
          *deadlineTick > state.battleTime.logicalTick
              ? *deadlineTick - state.battleTime.logicalTick
              : 0U;
      constexpr std::uint64_t millisecondsPerTick = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::nanoseconds{battle::BattleTime::tickNanos})
              .count());
      if (remainingTicks >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::chrono::milliseconds::rep>::max()) /
              millisecondsPerTick) {
        return std::nullopt;
      }
      activeTick = ActiveBattleTick{
          .battleId = load.battleId,
          .nextServerTick = movement.serverTick + 1U,
          .phase = lootPhase ? transport::tcp::BattleResumePhase::Loot
                             : transport::tcp::BattleResumePhase::Combat,
          .deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds{
                          static_cast<std::chrono::milliseconds::rep>(
                              remainingTicks * millisecondsPerTick)},
      };
    }

    return RecoveredRuntimeSeed{
        .roomId = load.roomId,
        .battleId = load.battleId,
        .continuityIdentity = continuityIdentity,
        .activeSessions = std::move(activeSessions),
        .participants = std::move(participants),
        .movement =
            battle::StateSnapshotProjection{
                .battleId = movement.battleId,
                .snapshotSequence = 0U,
                .serverTick = movement.serverTick,
                .players = movement.players,
            },
        .combat = recovered.battle.combatProjection(),
        .loot = recovered.battle.lootProjection(),
        .finalResult = result,
        .activeTick = std::move(activeTick),
    };
  }

  void applyRecoveredRuntimeSeed(
      RecoveredRuntimeSeed seed,
      std::chrono::steady_clock::time_point reconnectExpiresAt) {
    auto &members = roomMembers_[seed.roomId];
    for (const auto &session : seed.activeSessions) {
      members.insert(session.sessionId);
      sessionRoom_.insert_or_assign(session.sessionId, seed.roomId);
      detachedBattleSessions_.insert_or_assign(
          session.sessionId,
          DetachedBattleSession{.session = session,
                                .roomId = seed.roomId,
                                .battleId = seed.battleId,
                                .expiresAt = reconnectExpiresAt,
                                .resuming = false});
    }
    if (seed.activeTick.has_value()) {
      activeBattles_.insert_or_assign(seed.roomId, *seed.activeTick);
    }
    battleStateCache_.insert_or_assign(
        BattleKey{seed.roomId, seed.battleId},
        BattleStateCache{.roomId = seed.roomId,
                         .battleId = seed.battleId,
                         .continuityIdentity = seed.continuityIdentity,
                         .participants = std::move(seed.participants),
                         .movement = std::move(seed.movement),
                         .combat = std::move(seed.combat),
                         .loot = std::move(seed.loot),
                         .finalResult = std::move(seed.finalResult)});
  }

  bool stop() {
    if (!started_) {
      cleanupAfterFailedStart();
      return true;
    }
    started_ = false;
    bool clean = lifecycle_.requestStop();
    if (metricsServer_ != nullptr) {
      metricsServer_->stop();
    }

    if (reactor_ != nullptr) {
      static_cast<void>(reactor_->unwatch(tcpListener_.get()));
      static_cast<void>(reactor_->unwatch(udpSocket_.get()));
    }
    tcpListener_.reset();
    udpSocket_.reset();

    std::vector<int> descriptors;
    descriptors.reserve(connections_.size());
    for (const auto &[descriptor, state] : connections_) {
      static_cast<void>(state);
      descriptors.push_back(descriptor);
    }
    for (const int descriptor : descriptors) {
      beginDisconnect(descriptor);
    }

    if (deadlines_ != nullptr) {
      deadlines_->stop();
    }
    if (workers_ != nullptr) {
      clean = workers_->waitUntilIdle(kShutdownTimeout) && clean;
    }
    if (continuityStorage_ != nullptr) {
      continuityStorage_->waitUntilIdle();
    }
    if (workers_ != nullptr) {
      clean = workers_->waitUntilIdle(kShutdownTimeout) && clean;
    }
    if (observedStorage_ != nullptr) {
      clean = observedStorage_->waitUntilIdle(kShutdownTimeout) && clean;
    }
    if (workers_ != nullptr) {
      clean = workers_->waitUntilIdle(kShutdownTimeout) && clean;
    }
    if (publisherDriver_ != nullptr) {
      publisherDriver_->stop();
    }
    if (claimClient_ != nullptr) {
      clean = claimClient_->waitUntilIdle(kShutdownTimeout) && clean;
      claimClient_->stop();
    }

    for (auto &[descriptor, state] : connections_) {
      static_cast<void>(state.connection.markClosed());
      if (reactor_ != nullptr) {
        static_cast<void>(reactor_->unwatch(descriptor));
      }
      ::close(descriptor);
    }
    connections_.clear();
    epochToDescriptor_.clear();
    sessionToEpoch_.clear();
    roomMembers_.clear();
    sessionRoom_.clear();
    activeBattles_.clear();
    detachedBattleSessions_.clear();
    pendingResumes_.clear();
    battleStateCache_.clear();
    pendingBattleRetirements_.clear();
    resumeAwaitingProjection_.clear();
    resumeSnapshotAcks_.clear();

    if (workers_ != nullptr) {
      const auto metrics = workers_->metrics();
      clean = metrics.pendingTasks == 0u && metrics.activeTasks == 0u && clean;
    }

    battleLoadFlow_.reset();
    lobbyFlow_.reset();
    combatFlow_.reset();
    movementFlow_.reset();
    gateway_.reset();
    readiness_.reset();
    authFlow_.reset();
    correlations_.reset();
    claimClient_.reset();
    publisherDriver_.reset();
    publisher_.reset();
    settlementClient_.reset();
    metricsServer_.reset();
    observedStorage_.reset();
    gameMetrics_.reset();
    storageWorker_.reset();
    if (continuityStorage_ != nullptr) {
      continuityStorage_->stop();
    }
    continuityStorage_.reset();
    deadlines_.reset();
    if (workers_ != nullptr) {
      workers_->stop();
    }
    workers_.reset();
    reactor_.reset();

    clean = lifecycle_.markStopped() && clean;
    std::cout << "STOPPED connections=0 pendingTasks=0 activeTasks=0\n"
              << std::flush;
    return clean;
  }

  void cleanupAfterFailedStart() {
    if (lifecycle_.phase() == runtime::ProcessPhase::Running) {
      static_cast<void>(lifecycle_.requestStop());
    }
    if (publisherDriver_ != nullptr) {
      publisherDriver_->stop();
    }
    if (metricsServer_ != nullptr) {
      metricsServer_->stop();
    }
    if (claimClient_ != nullptr) {
      claimClient_->stop();
    }
    if (deadlines_ != nullptr) {
      deadlines_->stop();
    }
    if (workers_ != nullptr) {
      static_cast<void>(workers_->waitUntilIdle(kShutdownTimeout));
    }
    battleLoadFlow_.reset();
    lobbyFlow_.reset();
    combatFlow_.reset();
    movementFlow_.reset();
    gateway_.reset();
    authFlow_.reset();
    correlations_.reset();
    claimClient_.reset();
    publisherDriver_.reset();
    publisher_.reset();
    settlementClient_.reset();
    metricsServer_.reset();
    observedStorage_.reset();
    gameMetrics_.reset();
    storageWorker_.reset();
    if (continuityStorage_ != nullptr) {
      continuityStorage_->stop();
    }
    continuityStorage_.reset();
    deadlines_.reset();
    if (workers_ != nullptr) {
      workers_->stop();
    }
    workers_.reset();
    reactor_.reset();
    tcpListener_.reset();
    udpSocket_.reset();
    if (lifecycle_.phase() == runtime::ProcessPhase::StopRequested) {
      static_cast<void>(lifecycle_.markStopped());
    }
  }

  void waitForIngress() {
    if (reactor_ != nullptr) {
      const auto events = reactor_->wait(kIngressPollInterval);
      for (const auto &event : events) {
        if (event.kind != runtime::linux::ReadyKind::Socket) {
          continue;
        }
        if (event.fileDescriptor == tcpListener_.get()) {
          acceptConnections();
        } else if (event.fileDescriptor == udpSocket_.get()) {
          receiveDatagrams();
        } else {
          handleConnectionEvent(event.fileDescriptor, event.readable,
                                event.writable, event.error || event.hangup);
        }
      }
      return;
    }

    std::vector<pollfd> descriptors;
    descriptors.reserve(connections_.size() + 2u);
    descriptors.push_back(
        pollfd{.fd = tcpListener_.get(), .events = POLLIN, .revents = 0});
    descriptors.push_back(
        pollfd{.fd = udpSocket_.get(), .events = POLLIN, .revents = 0});
    for (const auto &[descriptor, state] : connections_) {
      short events = POLLIN;
      if (!state.connection.pendingOutbound().empty()) {
        events = static_cast<short>(events | POLLOUT);
      }
      descriptors.push_back(
          pollfd{.fd = descriptor, .events = events, .revents = 0});
    }
    const int ready =
        ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()),
               static_cast<int>(kIngressPollInterval.count()));
    if (ready <= 0) {
      return;
    }
    if ((descriptors[0].revents & POLLIN) != 0) {
      acceptConnections();
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
      receiveDatagrams();
    }
    for (std::size_t index = 2u; index < descriptors.size(); ++index) {
      const auto revents = descriptors[index].revents;
      handleConnectionEvent(descriptors[index].fd, (revents & POLLIN) != 0,
                            (revents & POLLOUT) != 0,
                            (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0);
    }
  }

  void acceptConnections() {
    while (true) {
      const auto accepted =
          transport::tcp::TcpSocketIo::acceptNonBlocking(tcpListener_.get());
      if (accepted.status == transport::tcp::IoStatus::WouldBlock) {
        return;
      }
      if (accepted.status == transport::tcp::IoStatus::Interrupted) {
        continue;
      }
      if (accepted.status != transport::tcp::IoStatus::Progress ||
          accepted.fileDescriptor < 0) {
        fatalStop_.store(true, std::memory_order_release);
        return;
      }
      if (connections_.size() >= config_.maxConnections) {
        ::close(accepted.fileDescriptor);
        continue;
      }
      const auto epoch = nextConnectionEpoch_++;
      connections_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(accepted.fileDescriptor),
          std::forward_as_tuple(epoch, std::chrono::steady_clock::now()));
      epochToDescriptor_.emplace(epoch, accepted.fileDescriptor);
      if (reactor_ != nullptr &&
          !reactor_->watch(accepted.fileDescriptor, false)) {
        closeConnection(accepted.fileDescriptor);
        fatalStop_.store(true, std::memory_order_release);
        return;
      }
    }
  }

  void handleConnectionEvent(int descriptor, bool readable, bool writable,
                             bool failed) {
    if (!connections_.contains(descriptor)) {
      return;
    }
    if (failed) {
      closeConnection(descriptor);
      return;
    }
    if (readable) {
      receiveTcp(descriptor);
    }
    if (connections_.contains(descriptor) && writable) {
      flushTcp(descriptor);
    }
  }

  void receiveTcp(int descriptor) {
    std::array<std::byte, 8192> bytes{};
    while (connections_.contains(descriptor)) {
      const auto received =
          transport::tcp::TcpSocketIo::receiveNonBlocking(descriptor, bytes);
      if (received.status == transport::tcp::IoStatus::Interrupted) {
        continue;
      }
      if (received.status == transport::tcp::IoStatus::WouldBlock) {
        return;
      }
      if (received.status != transport::tcp::IoStatus::Progress ||
          received.bytes == 0u) {
        closeConnection(descriptor);
        return;
      }
      auto &state = connections_.at(descriptor);
      const auto chunk = std::span{bytes}.first(received.bytes);
      if (state.connection.phase() ==
              transport::tcp::ConnectionPhase::ConnectedUnauthenticated ||
          state.connection.phase() ==
              transport::tcp::ConnectionPhase::AwaitingClaim) {
        state.connection.onBytes(
            chunk, std::chrono::steady_clock::now(),
            transport::tcp::SessionProtocolCodec::decodePreAuthPayload,
            [this, epoch = state.epoch](
                transport::tcp::NormalizedAuthRequest request) {
              const auto route = epochToDescriptor_.find(epoch);
              if (route == epochToDescriptor_.end()) {
                return;
              }
              auto &connection = connections_.at(route->second);
              connection.resumeRequestId =
                  request.resumeRequested
                      ? std::optional<std::uint64_t>{request.requestId}
                      : std::nullopt;
              for (auto &frame : authFlow_->begin(epoch, request)) {
                applySessionFrame(std::move(frame));
              }
            });
      } else if (state.connection.phase() ==
                 transport::tcp::ConnectionPhase::Authenticated) {
        state.postAuthInbound.insert(state.postAuthInbound.end(), chunk.begin(),
                                     chunk.end());
        dispatchPostAuthFrames(descriptor);
      }
      if (state.connection.phase() ==
              transport::tcp::ConnectionPhase::Closing &&
          state.connection.pendingOutbound().empty()) {
        closeConnection(descriptor);
        return;
      }
    }
  }

  void dispatchPostAuthFrames(int descriptor) {
    auto &state = connections_.at(descriptor);
    while (state.postAuthInbound.size() >= 4u) {
      std::uint32_t payloadBytes{};
      for (std::size_t index = 0u; index < 4u; ++index) {
        payloadBytes = (payloadBytes << 8u) | std::to_integer<std::uint32_t>(
                                                  state.postAuthInbound[index]);
      }
      const std::size_t frameBytes = 4u + payloadBytes;
      if (payloadBytes == 0u || payloadBytes > kMaximumTcpFrameBytes) {
        static_cast<void>(state.connection.beginClose(
            transport::tcp::CloseReason::FrameTooLarge));
        return;
      }
      if (state.postAuthInbound.size() < frameBytes) {
        return;
      }
      std::vector<std::byte> frame{state.postAuthInbound.begin(),
                                   state.postAuthInbound.begin() +
                                       static_cast<std::ptrdiff_t>(frameBytes)};
      state.postAuthInbound.erase(state.postAuthInbound.begin(),
                                  state.postAuthInbound.begin() +
                                      static_cast<std::ptrdiff_t>(frameBytes));
      if (!dispatchPostAuthFrame(state, frame)) {
        static_cast<void>(state.connection.beginClose(
            transport::tcp::CloseReason::MalformedFrame));
        return;
      }
    }
  }

  bool dispatchPostAuthFrame(ConnectionState &state,
                             std::span<const std::byte> frame) {
    if (!state.session.has_value()) {
      return false;
    }
    const auto sessionMessage =
        transport::tcp::SessionProtocolCodec::decodeFrame(frame);
    if (sessionMessage.message.has_value()) {
      if (const auto *applied =
              std::get_if<transport::tcp::BattleResumeSnapshotApplied>(
                  &*sessionMessage.message)) {
        if (!state.pendingResumeSnapshotId.has_value() ||
            *state.pendingResumeSnapshotId != applied->snapshotId) {
          return false;
        }
        const auto sessionId = state.session->sessionId;
        const auto expected = resumeSnapshotAcks_.find(sessionId);
        if (expected == resumeSnapshotAcks_.end() ||
            expected->second != applied->snapshotId) {
          return false;
        }
        state.pendingResumeSnapshotId.reset();
        resumeSnapshotAcks_.erase(expected);
        const auto detached = detachedBattleSessions_.find(sessionId);
        if (detached != detachedBattleSessions_.end()) {
          const BattleKey key{detached->second.roomId,
                              detached->second.battleId};
          detachedBattleSessions_.erase(detached);
          cleanupBattleCacheIfUnused(key);
        }
        return true;
      }
      if (const auto *request =
              std::get_if<transport::tcp::RequestRudpBindCapability>(
                  &*sessionMessage.message)) {
        if (resumeAwaitingProjection_.contains(state.session->sessionId) ||
            resumeSnapshotAcks_.contains(state.session->sessionId)) {
          return false;
        }
        auto outbound = authFlow_->requestRudpBindCapability(
            state.epoch, *request, std::chrono::steady_clock::now());
        if (!outbound.has_value()) {
          return false;
        }
        applySessionFrame(std::move(*outbound));
        return true;
      }
      return false;
    }
    const auto lobby = lobbyFlow_->submit(*state.session, frame);
    if (lobby.codecError == transport::tcp::LobbyRoomCodecError::None &&
        lobby.submitResult.has_value()) {
      return true;
    }
    const auto battle = battleLoadFlow_->submit(*state.session, frame);
    return battle.codecError == transport::tcp::BattleLoadCodecError::None &&
           battle.submitResult.has_value();
  }

  void flushTcp(int descriptor) {
    auto &state = connections_.at(descriptor);
    while (!state.connection.pendingOutbound().empty()) {
      const auto sent = transport::tcp::TcpSocketIo::sendNonBlocking(
          descriptor, state.connection.pendingOutbound());
      if (sent.status == transport::tcp::IoStatus::Interrupted) {
        continue;
      }
      if (sent.status == transport::tcp::IoStatus::WouldBlock) {
        break;
      }
      if (sent.status != transport::tcp::IoStatus::Progress ||
          !state.connection.consumeOutbound(sent.bytes)) {
        closeConnection(descriptor);
        return;
      }
    }
    if (!connections_.contains(descriptor)) {
      return;
    }
    if (reactor_ != nullptr) {
      static_cast<void>(reactor_->watch(
          descriptor, !state.connection.pendingOutbound().empty()));
    }
    if (state.connection.pendingOutbound().empty() &&
        state.connection.phase() == transport::tcp::ConnectionPhase::Closing) {
      closeConnection(descriptor);
    }
  }

  void queueTcp(std::uint64_t epoch, std::span<const std::byte> frame) {
    const auto route = epochToDescriptor_.find(epoch);
    if (route == epochToDescriptor_.end()) {
      return;
    }
    auto &state = connections_.at(route->second);
    if (!state.connection.queueOutbound(frame)) {
      static_cast<void>(
          state.connection.beginClose(transport::tcp::CloseReason::SlowWriter));
      return;
    }
    if (reactor_ != nullptr) {
      static_cast<void>(reactor_->watch(route->second, true));
    }
  }

  void processClaimCompletions() {
    for (auto &completion : claimCompletions_.takeAll()) {
      const auto identity = completion.identity();
      const auto now = std::chrono::steady_clock::now();
      auto frames =
          authFlow_->complete(completion, unixTimeMilliseconds(), now);
      std::optional<game_flow::AuthenticatedRoomSession> authenticated;
      std::uint64_t authenticatedEpoch{};
      if (identity.has_value()) {
        for (const auto &frame : frames) {
          if (frame.transition != ConnectionTransition::MarkAuthenticated) {
            continue;
          }
          const auto decoded =
              transport::tcp::SessionProtocolCodec::decodeFrame(frame.frame);
          const auto *welcome =
              decoded.message.has_value()
                  ? std::get_if<transport::tcp::Welcome>(&*decoded.message)
                  : nullptr;
          if (welcome != nullptr) {
            authenticatedEpoch = frame.connectionEpoch;
            authenticated = game_flow::AuthenticatedRoomSession{
                .accountId = shared::AccountId{identity->accountId},
                .sessionId = shared::SessionId{welcome->sessionId},
                .generation =
                    shared::SessionGeneration{welcome->sessionGeneration},
                .nickname = welcome->nickname,
            };
          }
        }
      }
      bool resumed = false;
      if (authenticated.has_value()) {
        const auto descriptor = epochToDescriptor_.find(authenticatedEpoch);
        if (descriptor != epochToDescriptor_.end()) {
          auto &state = connections_.at(descriptor->second);
          resumed = state.resumeRequestId.has_value();
          if (resumed) {
            const auto detached =
                detachedBattleSessions_.find(authenticated->sessionId);
            if (detached == detachedBattleSessions_.end() ||
                detached->second.session.generation !=
                    authenticated->generation ||
                detached->second.session.accountId !=
                    authenticated->accountId) {
              resumed = false;
              authenticated.reset();
            } else {
              detached->second.resuming = true;
              pendingResumes_.insert_or_assign(
                  authenticated->sessionId,
                  PendingResume{.connectionEpoch = authenticatedEpoch,
                                .requestId = *state.resumeRequestId});
              resumeAwaitingProjection_.insert(authenticated->sessionId);
            }
          }
          if (authenticated.has_value()) {
            state.session = authenticated;
            state.resumeRequestId.reset();
            sessionToEpoch_.insert_or_assign(authenticated->sessionId,
                                             authenticatedEpoch);
          }
        }
      }
      for (auto &frame : frames) {
        applySessionFrame(std::move(frame));
      }
      if (!authenticated.has_value()) {
        const auto descriptor = epochToDescriptor_.find(authenticatedEpoch);
        if (descriptor != epochToDescriptor_.end()) {
          closeConnection(descriptor->second);
        }
        continue;
      }
      if (resumed) {
        const BattleKey key{
            detachedBattleSessions_.at(authenticated->sessionId).roomId,
            detachedBattleSessions_.at(authenticated->sessionId).battleId};
        const auto cached = battleStateCache_.find(key);
        if (cached != battleStateCache_.end() &&
            cached->second.finalResult.has_value()) {
          queueResumeSnapshot(authenticated->sessionId, nullptr);
        } else if (gateway_->resumeBattleInput(authenticated->sessionId,
                                               authenticated->generation) !=
                   game_flow::RoomSubmitResult::Accepted) {
          const auto descriptor = epochToDescriptor_.find(authenticatedEpoch);
          if (descriptor != epochToDescriptor_.end()) {
            closeConnection(descriptor->second);
          }
        }
        continue;
      }
      expireReplacedDetachedSession(*authenticated);
      if (!gateway_->enterLobby(*authenticated)) {
        const auto descriptor = epochToDescriptor_.find(authenticatedEpoch);
        if (descriptor != epochToDescriptor_.end()) {
          closeConnection(descriptor->second);
        }
      }
    }
  }

  void applySessionFrame(RoutedSessionFrame frame) {
    const auto route = epochToDescriptor_.find(frame.connectionEpoch);
    if (route == epochToDescriptor_.end()) {
      return;
    }
    auto &state = connections_.at(route->second);
    queueTcp(frame.connectionEpoch, frame.frame);
    if (frame.transition == ConnectionTransition::MarkAuthenticated) {
      if (!state.connection.markAuthenticated()) {
        closeConnection(route->second);
      }
    } else if (frame.transition == ConnectionTransition::CloseAfterWrite) {
      static_cast<void>(state.connection.beginClose(frame.closeReason));
    }
  }

  void processRoomOutbounds() {
    for (auto &intent : roomOutbounds_.takeAll()) {
      if (recovering_ &&
          std::holds_alternative<game_flow::BattleRecoveryNotice>(
              intent.message)) {
        recoveryFailed_ = true;
      }
      updateRoutingState(intent);
      auto encoded = LobbyRoomFlow::encode(intent);
        if (!encoded.has_value()) {
          auto battle = BattleLoadFlow::encode(intent);
          if (!battle.has_value()) {
            fatalStop_.store(true, std::memory_order_release);
            continue;
          }
        sendToAudience(battle->audience, battle->frame);
      } else {
        sendToAudience(encoded->audience, encoded->frame);
      }
    }
  }

  std::optional<shared::RoomId> roomForBattleParticipants(
      shared::BattleInstanceId battleId,
      std::span<const game_flow::BattleParticipantProjection> participants) {
    for (const auto &participant : participants) {
      const auto room = sessionRoom_.find(participant.sessionId);
      if (room == sessionRoom_.end()) {
        continue;
      }
      const auto active = activeBattles_.find(room->second);
      if ((active != activeBattles_.end() &&
           active->second.battleId == battleId) ||
          battleStateCache_.contains(BattleKey{room->second, battleId})) {
        return room->second;
      }
    }
    return std::nullopt;
  }

  void processMovementSnapshots() {
    for (auto &snapshot : movementSnapshots_.takeAll()) {
      if (gameMetrics_ != nullptr) {
        gameMetrics_->recordSnapshot(std::chrono::steady_clock::now());
      }
      for (const auto &player : snapshot.players) {
        const auto room = sessionRoom_.find(player.sessionId);
        if (room == sessionRoom_.end()) {
          continue;
        }
        const BattleKey key{room->second, snapshot.battleId};
        const auto active = activeBattles_.find(room->second);
        if ((active != activeBattles_.end() &&
             active->second.battleId == snapshot.battleId) ||
            battleStateCache_.contains(key)) {
          auto &cached = battleStateCache_[key];
          cached.roomId = room->second;
          cached.battleId = snapshot.battleId;
          cached.movement = snapshot;
          break;
        }
      }
      if (movementFlow_ == nullptr) {
        fatalStop_.store(true, std::memory_order_release);
        return;
      }
      auto datagrams = movementFlow_->encodeSnapshot(snapshot);
      if (!datagrams.has_value()) {
        fatalStop_.store(true, std::memory_order_release);
        return;
      }
      for (auto &datagram : *datagrams) {
        if (!movementDatagrams_.push(std::move(datagram))) {
          fatalStop_.store(true, std::memory_order_release);
          return;
        }
      }
    }
  }

  void processCombatOutbounds() {
    if (combatFlow_ == nullptr) {
      if (!combatOutbounds_.takeAll().empty()) {
        fatalStop_.store(true, std::memory_order_release);
      }
      return;
    }
    for (auto &intent : combatOutbounds_.takeAll()) {
      bool resume = false;
      std::visit(
          [this, &resume](const auto &message) {
            using Message = std::remove_cvref_t<decltype(message)>;
            if constexpr (std::is_same_v<
                              Message, game_flow::CombatBattleResumeOutbound>) {
              resume = true;
              const BattleKey key{message.projection.roomId,
                                  message.projection.battleId};
              auto &cached = battleStateCache_[key];
              cached.roomId = message.projection.roomId;
              cached.battleId = message.projection.battleId;
              cached.movement = battle::StateSnapshotProjection{
                  .battleId = message.projection.battleId,
                  .snapshotSequence = 0u,
                  .serverTick = message.projection.serverTick,
                  .players = {}};
              cached.movement->players.reserve(
                  message.projection.participants.size());
              for (const auto &participant : message.projection.participants) {
                cached.movement->players.push_back(
                    battle::PlayerPositionProjection{
                        .sessionId = participant.sessionId,
                        .posXMillimeter = participant.posXMillimeter,
                        .posYMillimeter = participant.posYMillimeter});
              }
              cached.combat = message.projection.combat;
              cached.loot = message.projection.loot;
              queueResumeSnapshot(message.projection.sessionId,
                                  &message.projection);
            } else if constexpr (std::is_same_v<
                                     Message,
                                     game_flow::CombatMonsterSpawnedOutbound>) {
              const auto room = roomForBattleParticipants(message.battleId,
                                                          message.participants);
              if (!room.has_value()) {
                return;
              }
              auto &cached =
                  battleStateCache_[BattleKey{*room, message.battleId}];
              cached.roomId = *room;
              cached.battleId = message.battleId;
              cached.participants = message.participants;
              cached.combat = battle::CombatProjection{
                  .battleId = message.battleId,
                  .monsterId = battle::CombatRuleset::monsterId,
                  .hitPoints =
                      battle::CombatRuleset::monsterHitPointsForParticipants(
                          static_cast<std::uint32_t>(
                              message.participants.size())),
                  .monsterState = battle::MonsterState::Alive,
                  .outcome = battle::CombatOutcome::None,
                  .terminal = std::nullopt,
                  .serverTick = 0u};
            } else if constexpr (std::is_same_v<
                                     Message,
                                     game_flow::CombatAttackAppliedOutbound>) {
              const auto room = roomForBattleParticipants(
                  message.applied.battleId, message.participants);
              if (!room.has_value()) {
                return;
              }
              auto &cached =
                  battleStateCache_[BattleKey{*room, message.applied.battleId}];
              cached.participants = message.participants;
              if (cached.combat.has_value()) {
                cached.combat->hitPoints = message.applied.remainingHitPoints;
                cached.combat->outcome = message.applied.outcome;
                cached.combat->serverTick = message.applied.serverTick;
              }
            } else if constexpr (std::is_same_v<
                                     Message,
                                     game_flow::CombatMonsterStateOutbound>) {
              const auto room = roomForBattleParticipants(
                  message.projection.battleId, message.participants);
              if (room.has_value()) {
                auto &cached = battleStateCache_[BattleKey{
                    *room, message.projection.battleId}];
                cached.roomId = *room;
                cached.battleId = message.projection.battleId;
                cached.participants = message.participants;
                cached.combat = message.projection;
              }
            } else if constexpr (std::is_same_v<
                                     Message,
                                     game_flow::CombatTerminalEventOutbound>) {
              const auto room = roomForBattleParticipants(
                  message.terminal.battleId, message.participants);
              if (room.has_value()) {
                auto &cached = battleStateCache_[BattleKey{
                    *room, message.terminal.battleId}];
                cached.participants = message.participants;
                if (cached.combat.has_value()) {
                  cached.combat->terminal = message.terminal;
                  cached.combat->outcome = message.terminal.outcome;
                  cached.combat->serverTick = message.terminal.serverTick;
                }
              }
            } else if constexpr (std::is_same_v<
                                     Message,
                                     game_flow::LootDropsSpawnedOutbound>) {
              const auto room = roomForBattleParticipants(
                  message.projection.battleId, message.participants);
              if (!room.has_value()) {
                return;
              }
              auto &cached = battleStateCache_[BattleKey{
                  *room, message.projection.battleId}];
              cached.roomId = *room;
              cached.battleId = message.projection.battleId;
              cached.participants = message.participants;
              cached.loot = message.projection;
              const auto active = activeBattles_.find(*room);
              if (active != activeBattles_.end() &&
                  active->second.battleId == message.projection.battleId) {
                active->second.phase = transport::tcp::BattleResumePhase::Loot;
                active->second.deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{
                        battle::RelicRuleset::resolutionWindowMillis};
              }
            } else if constexpr (std::is_same_v<Message,
                                                game_flow::LootStateOutbound>) {
              const auto room = roomForBattleParticipants(
                  message.projection.battleId, message.participants);
              if (room.has_value()) {
                auto &cached = battleStateCache_[BattleKey{
                    *room, message.projection.battleId}];
                cached.roomId = *room;
                cached.battleId = message.projection.battleId;
                cached.participants = message.participants;
                cached.loot = message.projection;
              }
            }
          },
          intent.message);
      if (!resume) {
        combatFlow_->handleCombatOutbound(std::move(intent));
      }
    }
  }

  void updateRoutingState(const game_flow::LobbyRoomOutboundIntent &intent) {
    if (const auto *entry =
            std::get_if<game_flow::LobbyEntrySnapshot>(&intent.message)) {
      removeSessionFromRoom(entry->session.sessionId);
    }
    if (std::holds_alternative<game_flow::LobbyRoomListUpdate>(
            intent.message)) {
      if (const auto *session =
              std::get_if<game_flow::SessionAudience>(&intent.audience)) {
        removeSessionFromRoom(session->sessionId);
      }
    }
    if (const auto *detail =
            std::get_if<lobby_room::RoomDetailProjection>(&intent.message)) {
      auto &members = roomMembers_[detail->roomId];
      for (const auto sessionId : members) {
        const auto route = sessionRoom_.find(sessionId);
        if (route != sessionRoom_.end() && route->second == detail->roomId) {
          sessionRoom_.erase(route);
        }
      }
      members.clear();
      for (const auto &member : detail->members) {
        members.insert(member.sessionId);
        sessionRoom_.insert_or_assign(member.sessionId, detail->roomId);
      }
      if (members.empty()) {
        roomMembers_.erase(detail->roomId);
      }
      cleanupBattleCachesForRoom(detail->roomId);
    }
    if (const auto *started =
            std::get_if<game_flow::ArenaGameplayStart>(&intent.message)) {
      const auto now = std::chrono::steady_clock::now();
      std::optional<battle_continuity::BattleIdentity> continuityIdentity;
      if (continuityStorage_ != nullptr) {
        const auto roomValue = started->roomId.value();
        const auto originRecoveryEpoch =
            static_cast<std::uint32_t>(roomValue >> 32U);
        if (originRecoveryEpoch == 0U ||
            static_cast<std::uint32_t>(roomValue) == 0U ||
            started->battleId.value() == 0U) {
          fatalStop_.store(true, std::memory_order_release);
          return;
        }
        continuityIdentity = battle_continuity::BattleIdentity{
            .originRecoveryEpoch = originRecoveryEpoch,
            .roomId = started->roomId,
            .battleInstanceId = started->battleId};
      }
      activeBattles_.insert_or_assign(
          started->roomId,
          ActiveBattleTick{
              .battleId = started->battleId,
              .nextServerTick = 1u,
              .phase = transport::tcp::BattleResumePhase::Combat,
              .deadline =
                  now + std::chrono::milliseconds{
                            battle::CombatRuleset::combatDeadlineMillis}});
      for (auto cache = battleStateCache_.begin();
           cache != battleStateCache_.end();) {
        if (cache->first.first == started->roomId &&
            cache->first.second != started->battleId) {
          const BattleKey staleKey = cache->first;
          ++cache;
          const bool detached = std::ranges::any_of(
              detachedBattleSessions_, [&staleKey](const auto &entry) {
                return entry.second.roomId == staleKey.first &&
                       entry.second.battleId == staleKey.second;
              });
          if (!detached) {
            static_cast<void>(retireAndEraseBattleCache(staleKey, true));
          }
        } else {
          ++cache;
        }
      }
      battleStateCache_.insert_or_assign(
          BattleKey{started->roomId, started->battleId},
          BattleStateCache{
              .roomId = started->roomId,
              .battleId = started->battleId,
              .continuityIdentity = continuityIdentity,
              .participants = started->participants,
              .movement = std::nullopt,
              .combat =
                  battle::CombatProjection{
                      .battleId = started->battleId,
                      .monsterId = battle::CombatRuleset::monsterId,
                      .hitPoints = battle::CombatRuleset::
                          monsterHitPointsForParticipants(
                              static_cast<std::uint32_t>(
                                  started->participants.size())),
                      .monsterState = battle::MonsterState::Alive,
                      .outcome = battle::CombatOutcome::None,
                      .terminal = std::nullopt,
                      .serverTick = 0u},
              .loot =
                  battle::LootProjection{
                      .battleId = started->battleId,
                      .resolution = battle::LootResolutionState::NotStarted,
                      .drops = {},
                      .holdings = {}},
              .finalResult = std::nullopt});
    }
    if (const auto *cancelled =
            std::get_if<game_flow::ArenaLoadCancelled>(&intent.message)) {
      const auto battle = activeBattles_.find(cancelled->roomId);
      if (battle != activeBattles_.end() &&
          battle->second.battleId == cancelled->battleId) {
        activeBattles_.erase(battle);
      }
      const BattleKey key{cancelled->roomId, cancelled->battleId};
      battleStateCache_.erase(key);
      pendingBattleRetirements_.erase(key);
    }
    if (const auto *result =
            std::get_if<battle::BattleFinalResult>(&intent.message)) {
      auto &cached =
          battleStateCache_[BattleKey{result->roomId, result->battleId}];
      cached.roomId = result->roomId;
      cached.battleId = result->battleId;
      if (continuityStorage_ != nullptr &&
          !cached.continuityIdentity.has_value()) {
        cached.continuityIdentity = continuityIdentityFor(
            BattleKey{result->roomId, result->battleId}, cached);
      }
      cached.finalResult = *result;
      const auto active = activeBattles_.find(result->roomId);
      if (active != activeBattles_.end() &&
          active->second.battleId == result->battleId) {
        activeBattles_.erase(active);
      }
      queueWaitingResultSnapshots(result->roomId, result->battleId);
      cleanupBattleCacheIfUnused(BattleKey{result->roomId, result->battleId});
    }
    if (const auto *recovery =
            std::get_if<game_flow::BattleRecoveryNotice>(&intent.message)) {
      const auto active = activeBattles_.find(recovery->roomId);
      if (active != activeBattles_.end() &&
          active->second.battleId == recovery->battleId) {
        activeBattles_.erase(active);
      }
      cleanupBattleCacheIfUnused(
          BattleKey{recovery->roomId, recovery->battleId});
    }
  }

  std::optional<battle_continuity::BattleIdentity>
  continuityIdentityFor(const BattleKey &key,
                        const BattleStateCache &cached) const noexcept {
    if (cached.continuityIdentity.has_value()) {
      return cached.continuityIdentity;
    }
    if (continuityStorage_ == nullptr) {
      return std::nullopt;
    }
    const auto roomValue = key.first.value();
    const auto originRecoveryEpoch =
        static_cast<std::uint32_t>(roomValue >> 32U);
    if (originRecoveryEpoch == 0U ||
        static_cast<std::uint32_t>(roomValue) == 0U ||
        key.second.value() == 0U) {
      return std::nullopt;
    }
    return battle_continuity::BattleIdentity{
        .originRecoveryEpoch = originRecoveryEpoch,
        .roomId = key.first,
        .battleInstanceId = key.second};
  }

  bool retireAndEraseBattleCache(const BattleKey &key,
                                 bool ignoreRoomMembers) {
    const auto cached = battleStateCache_.find(key);
    if (cached == battleStateCache_.end()) {
      pendingBattleRetirements_.erase(key);
      return true;
    }
    if (continuityStorage_ == nullptr ||
        !cached->second.finalResult.has_value()) {
      pendingBattleRetirements_.erase(key);
      battleStateCache_.erase(cached);
      return true;
    }

    const auto identity = continuityIdentityFor(key, cached->second);
    if (!identity.has_value() ||
        continuityStorage_->retireBattle(
            *identity, continuityStorage_->writerRecoveryEpoch()) !=
            battle_continuity_storage::StorageError::None) {
      auto &pending = pendingBattleRetirements_[key];
      pending = pending || ignoreRoomMembers;
      return false;
    }
    pendingBattleRetirements_.erase(key);
    battleStateCache_.erase(cached);
    return true;
  }

  void cleanupBattleCacheIfUnused(const BattleKey &key) {
    const auto active = activeBattles_.find(key.first);
    if (active != activeBattles_.end() &&
        active->second.battleId == key.second) {
      return;
    }
    const bool detached =
        std::ranges::any_of(detachedBattleSessions_, [&key](const auto &entry) {
          return entry.second.roomId == key.first &&
                 entry.second.battleId == key.second;
        });
    const auto cached = battleStateCache_.find(key);
    const bool roomMember =
        cached != battleStateCache_.end() &&
        std::ranges::any_of(
            cached->second.participants, [this, &key](const auto &participant) {
              const auto route = sessionRoom_.find(participant.sessionId);
              return route != sessionRoom_.end() && route->second == key.first;
            });
    const auto pending = pendingBattleRetirements_.find(key);
    const bool ignoreRoomMembers =
        pending != pendingBattleRetirements_.end() && pending->second;
    if (!detached && (!roomMember || ignoreRoomMembers)) {
      static_cast<void>(retireAndEraseBattleCache(key, ignoreRoomMembers));
    }
  }

  void cleanupBattleCachesForRoom(shared::RoomId roomId) {
    std::vector<BattleKey> keys;
    for (const auto &[key, cache] : battleStateCache_) {
      static_cast<void>(cache);
      if (key.first == roomId) {
        keys.push_back(key);
      }
    }
    for (const auto &key : keys) {
      cleanupBattleCacheIfUnused(key);
    }
  }

  void expireReplacedDetachedSession(
      const game_flow::AuthenticatedRoomSession &replacement) {
    std::vector<shared::SessionId> replaced;
    for (const auto &[sessionId, detached] : detachedBattleSessions_) {
      if (sessionId != replacement.sessionId &&
          detached.session.accountId == replacement.accountId) {
        replaced.push_back(sessionId);
      }
    }
    for (const auto sessionId : replaced) {
      const auto detached = detachedBattleSessions_.find(sessionId);
      if (detached == detachedBattleSessions_.end() ||
          !gateway_->disconnect(sessionId,
                                detached->second.session.generation)) {
        continue;
      }
      const BattleKey key{detached->second.roomId, detached->second.battleId};
      static_cast<void>(authFlow_->expireDetached(
          sessionId, detached->second.session.generation));
      removeSessionFromRoom(sessionId);
      detachedBattleSessions_.erase(detached);
      pendingResumes_.erase(sessionId);
      resumeAwaitingProjection_.erase(sessionId);
      resumeSnapshotAcks_.erase(sessionId);
      cleanupBattleCacheIfUnused(key);
    }
  }

  std::uint32_t
  remainingBattleMillis(shared::RoomId roomId,
                        shared::BattleInstanceId battleId,
                        std::chrono::steady_clock::time_point now) {
    const auto active = activeBattles_.find(roomId);
    if (active == activeBattles_.end() || active->second.battleId != battleId ||
        active->second.deadline <= now) {
      return 0u;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            active->second.deadline - now)
            .count();
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        remaining, std::numeric_limits<std::uint32_t>::max()));
  }

  std::optional<std::uint64_t>
  resumeScore(shared::SessionId sessionId, const battle::LootProjection &loot,
              const std::optional<transport::tcp::FinalResult> &result) {
    if (result.has_value()) {
      const auto entry = std::ranges::find_if(
          result->entries, [sessionId](const auto &candidate) {
            return candidate.sessionId == sessionId.value();
          });
      return entry == result->entries.end()
                 ? std::optional<std::uint64_t>{}
                 : std::optional<std::uint64_t>{entry->finalAssetValue};
    }
    const auto catalog = battle::RelicCatalog::v1Snapshot();
    std::uint64_t score{};
    for (const auto &holding : loot.holdings) {
      if (holding.sessionId != sessionId) {
        continue;
      }
      const auto unitValue = catalog.unitValueOf(holding.itemId);
      if (!unitValue.has_value() ||
          (*unitValue != 0u &&
           holding.quantity >
               std::numeric_limits<std::uint64_t>::max() / *unitValue)) {
        return std::nullopt;
      }
      const auto value = holding.quantity * *unitValue;
      if (score > std::numeric_limits<std::uint64_t>::max() - value) {
        return std::nullopt;
      }
      score += value;
    }
    return score;
  }

  std::optional<transport::tcp::BattleResumeSnapshot>
  buildResumeSnapshot(const DetachedBattleSession &detached,
                      std::uint64_t requestId, std::uint64_t snapshotId,
                      const battle::BattleResumeProjection *projection) {
    const BattleKey key{detached.roomId, detached.battleId};
    const auto cached = battleStateCache_.find(key);
    if (projection != nullptr &&
        (projection->roomId != detached.roomId ||
         projection->battleId != detached.battleId ||
         projection->sessionId != detached.session.sessionId ||
         projection->generation != detached.session.generation)) {
      return std::nullopt;
    }

    std::optional<transport::tcp::FinalResult> finalResult;
    if (cached != battleStateCache_.end() &&
        cached->second.finalResult.has_value()) {
      finalResult =
          LobbyRoomFlow::encodeFinalResult(*cached->second.finalResult);
      if (!finalResult.has_value()) {
        return std::nullopt;
      }
    }

    std::vector<transport::tcp::BattleResumePlayer> players;
    std::uint32_t serverTick{};
    if (projection != nullptr) {
      players.reserve(projection->participants.size());
      for (const auto &participant : projection->participants) {
        players.push_back(transport::tcp::BattleResumePlayer{
            .sessionId = participant.sessionId.value(),
            .positionXMillimeters = participant.posXMillimeter,
            .positionYMillimeters = participant.posYMillimeter,
            .healthKnown = false,
            .hitPoints = 0u,
            .maximumHitPoints = 0u,
            .alive = participant.exitStatus ==
                         battle::ParticipantExitStatus::GameplayEligible ||
                     participant.exitStatus ==
                         battle::ParticipantExitStatus::TerminalPresent});
      }
      serverTick = projection->serverTick;
    } else {
      if (cached == battleStateCache_.end()) {
        return std::nullopt;
      }
      players.reserve(cached->second.participants.size());
      for (const auto &participant : cached->second.participants) {
        std::int32_t x{};
        std::int32_t y{};
        if (cached->second.movement.has_value()) {
          const auto position = std::ranges::find_if(
              cached->second.movement->players,
              [sessionId = participant.sessionId](const auto &candidate) {
                return candidate.sessionId == sessionId;
              });
          if (position != cached->second.movement->players.end()) {
            x = position->posXMillimeter;
            y = position->posYMillimeter;
          }
          serverTick = cached->second.movement->serverTick;
        }
        bool alive = true;
        if (finalResult.has_value()) {
          const auto resultEntry = std::ranges::find_if(
              finalResult->entries,
              [sessionId = participant.sessionId](const auto &candidate) {
                return candidate.sessionId == sessionId.value();
              });
          if (resultEntry == finalResult->entries.end()) {
            return std::nullopt;
          }
          alive = resultEntry->exitStatus ==
                  transport::tcp::FinalResultExitStatus::TerminalPresent;
        }
        players.push_back(transport::tcp::BattleResumePlayer{
            .sessionId = participant.sessionId.value(),
            .positionXMillimeters = x,
            .positionYMillimeters = y,
            .healthKnown = false,
            .hitPoints = 0u,
            .maximumHitPoints = 0u,
            .alive = alive});
      }
    }
    if (players.size() < battle::CombatRuleset::minimumParticipants ||
        players.size() > battle::CombatRuleset::maximumParticipants) {
      return std::nullopt;
    }

    const battle::CombatProjection *combat =
        projection != nullptr && projection->combat.has_value()
            ? &*projection->combat
        : cached != battleStateCache_.end() && cached->second.combat.has_value()
            ? &*cached->second.combat
            : nullptr;
    const battle::LootProjection *loot =
        projection != nullptr ? &projection->loot
        : cached != battleStateCache_.end() && cached->second.loot.has_value()
            ? &*cached->second.loot
            : nullptr;
    if (combat == nullptr || loot == nullptr) {
      return std::nullopt;
    }

    std::optional<transport::tcp::BattleResumeMonster> monster{
        transport::tcp::BattleResumeMonster{
            .monsterId = combat->monsterId,
            .positionXMillimeters =
                battle::CombatRuleset::spawnPosition.xMillimeter,
            .positionYMillimeters =
                battle::CombatRuleset::spawnPosition.yMillimeter,
            .hitPoints = combat->hitPoints,
            .maximumHitPoints =
                battle::CombatRuleset::monsterHitPointsForParticipants(
                    static_cast<std::uint32_t>(players.size())),
            .state = static_cast<std::uint8_t>(combat->monsterState)}};
    std::vector<transport::tcp::BattleResumeDrop> drops;
    drops.reserve(loot->drops.size());
    for (const auto &drop : loot->drops) {
      drops.push_back(transport::tcp::BattleResumeDrop{
          .dropId = drop.dropId.value,
          .itemId = drop.itemId.value,
          .quantity = drop.quantity,
          .positionXMillimeters = drop.position.xMillimeter,
          .positionYMillimeters = drop.position.yMillimeter,
          .state = static_cast<std::uint8_t>(drop.state),
          .ownerSessionId = drop.owner.has_value() ? drop.owner->value() : 0u});
    }
    const auto score =
        resumeScore(detached.session.sessionId, *loot, finalResult);
    if (!score.has_value()) {
      return std::nullopt;
    }

    auto phase = transport::tcp::BattleResumePhase::Combat;
    if (finalResult.has_value()) {
      phase = transport::tcp::BattleResumePhase::Result;
    } else if (loot->resolution != battle::LootResolutionState::NotStarted ||
               combat->outcome != battle::CombatOutcome::None) {
      phase = transport::tcp::BattleResumePhase::Loot;
    }
    const auto now = std::chrono::steady_clock::now();
    return transport::tcp::BattleResumeSnapshot{
        .requestId = requestId,
        .snapshotId = snapshotId,
        .roomId = detached.roomId.value(),
        .battleInstanceId = detached.battleId.value(),
        .playerSessionId = detached.session.sessionId.value(),
        .sessionGeneration = detached.session.generation.value(),
        .phase = phase,
        .remainingMillis = phase == transport::tcp::BattleResumePhase::Result
                               ? 0u
                               : remainingBattleMillis(detached.roomId,
                                                       detached.battleId, now),
        .serverTick = serverTick,
        .players = std::move(players),
        .monster = std::move(monster),
        .drops = std::move(drops),
        .score = *score,
        .result = std::move(finalResult)};
  }

  void queueResumeSnapshot(shared::SessionId sessionId,
                           const battle::BattleResumeProjection *projection) {
    const auto pending = pendingResumes_.find(sessionId);
    const auto detached = detachedBattleSessions_.find(sessionId);
    if (pending == pendingResumes_.end() ||
        detached == detachedBattleSessions_.end()) {
      return;
    }
    const auto descriptor =
        epochToDescriptor_.find(pending->second.connectionEpoch);
    if (descriptor == epochToDescriptor_.end()) {
      return;
    }
    auto &state = connections_.at(descriptor->second);
    if (!state.session.has_value() || state.session->sessionId != sessionId ||
        state.session->generation != detached->second.session.generation) {
      closeConnection(descriptor->second);
      return;
    }
    const auto snapshotId = nextResumeSnapshotId_++;
    auto snapshot = buildResumeSnapshot(
        detached->second, pending->second.requestId, snapshotId, projection);
    auto frame =
        snapshot.has_value()
            ? transport::tcp::SessionProtocolCodec::encodeFrame(
                  transport::tcp::SessionControlMessage{std::move(*snapshot)})
            : std::nullopt;
    if (!frame.has_value()) {
      closeConnection(descriptor->second);
      return;
    }
    state.pendingResumeSnapshotId = snapshotId;
    resumeSnapshotAcks_.insert_or_assign(sessionId, snapshotId);
    queueTcp(pending->second.connectionEpoch, *frame);
    pendingResumes_.erase(pending);
    resumeAwaitingProjection_.erase(sessionId);
  }

  void queueWaitingResultSnapshots(shared::RoomId roomId,
                                   shared::BattleInstanceId battleId) {
    std::vector<shared::SessionId> waiting;
    for (const auto &[sessionId, detached] : detachedBattleSessions_) {
      if (detached.resuming && detached.roomId == roomId &&
          detached.battleId == battleId &&
          pendingResumes_.contains(sessionId)) {
        waiting.push_back(sessionId);
      }
    }
    for (const auto sessionId : waiting) {
      queueResumeSnapshot(sessionId, nullptr);
    }
  }

  void sendToAudience(const game_flow::LobbyRoomOutboundAudience &audience,
                      std::span<const std::byte> frame) {
    std::set<std::uint64_t> epochs;
    std::visit(
        [this, &epochs](const auto &target) {
          using Target = std::remove_cvref_t<decltype(target)>;
          if constexpr (std::is_same_v<Target, game_flow::SessionAudience>) {
            const auto route = sessionToEpoch_.find(target.sessionId);
            if (route != sessionToEpoch_.end() &&
                !resumeAwaitingProjection_.contains(target.sessionId)) {
              const auto descriptor = epochToDescriptor_.find(route->second);
              if (descriptor != epochToDescriptor_.end()) {
                const auto &connection = connections_.at(descriptor->second);
                if (connection.session.has_value() &&
                    connection.session->generation == target.generation) {
                  epochs.insert(route->second);
                }
              }
            }
          } else if constexpr (std::is_same_v<Target,
                                              game_flow::LobbyAudience>) {
            for (const auto &[sessionId, epoch] : sessionToEpoch_) {
              if (!sessionRoom_.contains(sessionId) &&
                  !resumeAwaitingProjection_.contains(sessionId)) {
                epochs.insert(epoch);
              }
            }
          } else {
            const auto room = roomMembers_.find(target.roomId);
            if (room != roomMembers_.end()) {
              for (const auto sessionId : room->second) {
                const auto route = sessionToEpoch_.find(sessionId);
                if (route != sessionToEpoch_.end() &&
                    !resumeAwaitingProjection_.contains(sessionId)) {
                  epochs.insert(route->second);
                }
              }
            }
          }
        },
        audience);
    for (const auto epoch : epochs) {
      queueTcp(epoch, frame);
    }
  }

  void processUdpOutbounds() {
    for (auto &datagram : movementDatagrams_.takeAll()) {
      sendDatagram(datagram);
    }
    if (combatFlow_ == nullptr) {
      return;
    }
    auto reliable = combatFlow_->pollReliable(std::chrono::steady_clock::now());
    for (const auto &datagram : reliable.transmissions) {
      const auto sentAt = std::chrono::steady_clock::now();
      const bool succeeded = sendDatagram(datagram);
      combatFlow_->recordSend(datagram, sentAt, succeeded);
    }
    for (const auto &failure : reliable.failures) {
      const auto closed = correlations_->closeRudpPeer(failure);
      if (closed.has_value()) {
        applyRudpSessionClosure(*closed);
      }
    }
    for (const auto &datagram : combatFlow_->takeUnreliableSnapshots()) {
      sendDatagram(datagram);
    }
  }

  void applyRudpSessionClosure(const RudpSessionClosure &closed) {
    if (closed.connectionEpoch.has_value()) {
      const auto route = epochToDescriptor_.find(*closed.connectionEpoch);
      if (route != epochToDescriptor_.end()) {
        closeConnection(route->second);
        return;
      }
    }
    static_cast<void>(
        gateway_->disconnect(closed.sessionId, closed.generation));
  }

  bool sendDatagram(const EncodedRudpDatagram &datagram) {
    const auto address = socketAddressFor(datagram.endpoint);
    if (!address.has_value()) {
      return false;
    }
    const auto sent = ::sendto(
        udpSocket_.get(), datagram.datagram.data(), datagram.datagram.size(), 0,
        reinterpret_cast<const sockaddr *>(&*address), sizeof(*address));
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      fatalStop_.store(true, std::memory_order_release);
    }
    return sent >= 0 &&
           static_cast<std::size_t>(sent) == datagram.datagram.size();
  }

  void receiveDatagrams() {
    std::array<std::byte, kMaximumUdpDatagramBytes> bytes{};
    for (std::size_t count = 0u; count < kMaximumDatagramsPerTurn; ++count) {
      sockaddr_in peer{};
      socklen_t peerBytes = sizeof(peer);
      const auto received =
          ::recvfrom(udpSocket_.get(), bytes.data(), bytes.size(), 0,
                     reinterpret_cast<sockaddr *>(&peer), &peerBytes);
      if (received < 0 && errno == EINTR) {
        --count;
        continue;
      }
      if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      if (received <= 0 || peer.sin_family != AF_INET) {
        if (received < 0) {
          fatalStop_.store(true, std::memory_order_release);
        }
        return;
      }
      const auto datagram =
          std::span{bytes}.first(static_cast<std::size_t>(received));
      dispatchDatagram(datagram, endpointFor(peer));
    }
  }

  void dispatchDatagram(std::span<const std::byte> datagram,
                        const transport::rudp::RudpEndpoint &endpoint) {
    const auto decodedHeader =
        transport::rudp::RudpHeaderCodec::decode(datagram);
    if (decodedHeader.error != transport::rudp::RudpHeaderError::None ||
        !decodedHeader.header.has_value()) {
      return;
    }
    const auto &header = *decodedHeader.header;
    if (resumeAwaitingProjection_.contains(
            shared::SessionId{header.sessionId}) ||
        resumeSnapshotAcks_.contains(shared::SessionId{header.sessionId})) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (header.messageId == 22u) {
      const auto control = transport::rudp::RudpControlCodec::decode(datagram);
      const auto *hello =
          control.message.has_value()
              ? std::get_if<transport::rudp::RudpBindHello>(&*control.message)
              : nullptr;
      if (hello == nullptr) {
        return;
      }
      const auto bound = bindings_.bind(header, *hello, endpoint, now);
      if (bound.status != transport::rudp::RudpBindStatus::Accepted) {
        return;
      }
      auto response = transport::rudp::RudpControlCodec::encode(
          transport::rudp::RudpHeader{
              .flag = transport::rudp::RudpFlag::Reliable,
              .sessionId = header.sessionId,
              .sessionGeneration = header.sessionGeneration,
              .transportEpoch = bound.transportEpoch,
              .sequence = bound.sequence,
              .ack = bound.ack.ack,
              .ackBits = bound.ack.ackBits,
              .messageId = 23u,
          },
          transport::rudp::RudpControlMessage{
              transport::rudp::RudpBindAccepted{}});
      if (response.has_value()) {
        sendDatagram(EncodedRudpDatagram{.endpoint = endpoint,
                                         .datagram = std::move(*response)});
      }
      return;
    }
    if (header.flag == transport::rudp::RudpFlag::AckOnly ||
        header.messageId == 24u) {
      const auto received = bindings_.receive(header, endpoint, now);
      if (received.status == transport::rudp::RudpPacketStatus::Current &&
          combatFlow_ != nullptr) {
        static_cast<void>(combatFlow_->discardAcknowledged(
            header.sessionId, header.sessionGeneration, header.transportEpoch,
            header.ack, header.ackBits, now));
      }
      return;
    }
    if (header.messageId == 25u && movementFlow_ != nullptr) {
      const auto submitted = movementFlow_->submitMove(datagram, endpoint, now);
      if (combatFlow_ != nullptr &&
          (submitted == RudpMovementSubmitResult::Accepted ||
           submitted == RudpMovementSubmitResult::StaleTransport ||
           submitted == RudpMovementSubmitResult::RoomRejected)) {
        static_cast<void>(combatFlow_->discardAcknowledged(
            header.sessionId, header.sessionGeneration, header.transportEpoch,
            header.ack, header.ackBits, now));
      }
    } else if (header.messageId == 27u && combatFlow_ != nullptr) {
      static_cast<void>(combatFlow_->submitAttack(datagram, endpoint, now));
    } else if (header.messageId == 32u && combatFlow_ != nullptr) {
      static_cast<void>(combatFlow_->submitClaimLoot(datagram, endpoint, now));
    }
  }

  void processPeriodicWork() {
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextMovementTick_) {
      nextMovementTick_ = now + kMovementTickInterval;
      for (auto &[roomId, active] : activeBattles_) {
        static_cast<void>(gateway_->submitMovementTick(
            roomId, active.battleId, active.nextServerTick++));
      }
      if (observedStorage_ != nullptr) {
        observedStorage_->refresh();
      }
    }
    if (now >= nextRudpExpiry_) {
      nextRudpExpiry_ = now + kRudpExpiryInterval;
      for (const auto &expired : correlations_->expireTimedOutRudpPeers(now)) {
        applyRudpSessionClosure(expired);
      }
    }
    std::vector<shared::SessionId> reconnectExpired;
    for (const auto &[sessionId, detached] : detachedBattleSessions_) {
      if (!detached.resuming && now >= detached.expiresAt) {
        reconnectExpired.push_back(sessionId);
      }
    }
    for (const auto sessionId : reconnectExpired) {
      const auto detached = detachedBattleSessions_.find(sessionId);
      if (detached == detachedBattleSessions_.end() ||
          !gateway_->disconnect(sessionId,
                                detached->second.session.generation)) {
        continue;
      }
      const BattleKey key{detached->second.roomId, detached->second.battleId};
      static_cast<void>(authFlow_->expireDetached(
          sessionId, detached->second.session.generation));
      removeSessionFromRoom(sessionId);
      pendingResumes_.erase(sessionId);
      resumeAwaitingProjection_.erase(sessionId);
      resumeSnapshotAcks_.erase(sessionId);
      detachedBattleSessions_.erase(detached);
      cleanupBattleCacheIfUnused(key);
    }
    if (!pendingBattleRetirements_.empty()) {
      std::vector<BattleKey> pending;
      pending.reserve(pendingBattleRetirements_.size());
      for (const auto &[key, ignoreRoomMembers] : pendingBattleRetirements_) {
        static_cast<void>(ignoreRoomMembers);
        pending.push_back(key);
      }
      for (const auto &key : pending) {
        cleanupBattleCacheIfUnused(key);
      }
    }
    for (auto &[descriptor, state] : connections_) {
      static_cast<void>(descriptor);
      state.connection.onTimer(now);
    }
  }

  void closeDrainedConnections() {
    std::vector<int> closed;
    for (const auto &[descriptor, state] : connections_) {
      if (state.connection.phase() ==
              transport::tcp::ConnectionPhase::Closing &&
          state.connection.pendingOutbound().empty()) {
        closed.push_back(descriptor);
      }
    }
    for (const int descriptor : closed) {
      closeConnection(descriptor);
    }
  }

  void beginDisconnect(int descriptor) {
    const auto connection = connections_.find(descriptor);
    if (connection == connections_.end()) {
      return;
    }
    if (connection->second.session.has_value()) {
      const auto &session = *connection->second.session;
      const auto detached = detachedBattleSessions_.find(session.sessionId);
      std::optional<shared::RoomId> battleRoom;
      std::optional<shared::BattleInstanceId> battleId;
      auto expiresAt = std::chrono::steady_clock::now() + kBattleReconnectGrace;
      if (detached != detachedBattleSessions_.end()) {
        battleRoom = detached->second.roomId;
        battleId = detached->second.battleId;
        expiresAt = detached->second.expiresAt;
      } else {
        const auto room = sessionRoom_.find(session.sessionId);
        if (room != sessionRoom_.end()) {
          const auto active = activeBattles_.find(room->second);
          if (active != activeBattles_.end()) {
            battleRoom = room->second;
            battleId = active->second.battleId;
          } else {
            std::optional<BattleKey> latestFinal;
            for (const auto &[key, cached] : battleStateCache_) {
              if (key.first != room->second ||
                  !cached.finalResult.has_value() ||
                  !std::ranges::any_of(
                      cached.participants,
                      [sessionId = session.sessionId](const auto &participant) {
                        return participant.sessionId == sessionId;
                      })) {
                continue;
              }
              if (!latestFinal.has_value() ||
                  key.second > latestFinal->second) {
                latestFinal = key;
              }
            }
            if (latestFinal.has_value()) {
              battleRoom = room->second;
              battleId = latestFinal->second;
            }
          }
        }
      }
      const auto active = battleRoom.has_value()
                              ? activeBattles_.find(*battleRoom)
                              : activeBattles_.end();
      const bool needsSuspend = active != activeBattles_.end() &&
                                battleId.has_value() &&
                                active->second.battleId == *battleId;
      const bool suspendAccepted =
          !needsSuspend ||
          gateway_->suspendBattleInput(session.sessionId, session.generation) ==
              game_flow::RoomSubmitResult::Accepted;
      if (battleRoom.has_value() && battleId.has_value() && suspendAccepted &&
          authFlow_->detach(connection->second.epoch, expiresAt)) {
        detachedBattleSessions_.insert_or_assign(
            session.sessionId, DetachedBattleSession{.session = session,
                                                     .roomId = *battleRoom,
                                                     .battleId = *battleId,
                                                     .expiresAt = expiresAt,
                                                     .resuming = false});
        const auto current = sessionToEpoch_.find(session.sessionId);
        if (current != sessionToEpoch_.end() &&
            current->second == connection->second.epoch) {
          sessionToEpoch_.erase(current);
        }
        pendingResumes_.erase(session.sessionId);
        resumeAwaitingProjection_.erase(session.sessionId);
        resumeSnapshotAcks_.erase(session.sessionId);
        epochToDescriptor_.erase(connection->second.epoch);
        return;
      }
      static_cast<void>(
          gateway_->disconnect(session.sessionId, session.generation));
      const auto current = sessionToEpoch_.find(session.sessionId);
      if (current != sessionToEpoch_.end() &&
          current->second == connection->second.epoch) {
        removeSessionFromRoom(session.sessionId);
        sessionToEpoch_.erase(current);
      }
      if (detached != detachedBattleSessions_.end()) {
        const BattleKey key{detached->second.roomId, detached->second.battleId};
        detachedBattleSessions_.erase(detached);
        pendingResumes_.erase(session.sessionId);
        resumeAwaitingProjection_.erase(session.sessionId);
        resumeSnapshotAcks_.erase(session.sessionId);
        cleanupBattleCacheIfUnused(key);
      }
    }
    static_cast<void>(authFlow_->disconnect(connection->second.epoch));
    epochToDescriptor_.erase(connection->second.epoch);
  }

  void closeConnection(int descriptor) {
    const auto connection = connections_.find(descriptor);
    if (connection == connections_.end()) {
      return;
    }
    beginDisconnect(descriptor);
    static_cast<void>(connection->second.connection.markClosed());
    if (reactor_ != nullptr) {
      static_cast<void>(reactor_->unwatch(descriptor));
    }
    ::close(descriptor);
    connections_.erase(connection);
  }

  void removeSessionFromRoom(shared::SessionId sessionId) {
    const auto room = sessionRoom_.find(sessionId);
    if (room == sessionRoom_.end()) {
      return;
    }
    const auto roomId = room->second;
    const auto members = roomMembers_.find(room->second);
    if (members != roomMembers_.end()) {
      members->second.erase(sessionId);
      if (members->second.empty()) {
        roomMembers_.erase(members);
      }
    }
    sessionRoom_.erase(room);
    cleanupBattleCachesForRoom(roomId);
  }

  ServerConfig config_;
  settlement_storage::JournalRecoveryResult recovery_;
  std::unique_ptr<battle_continuity_storage::ContinuityStorage>
      continuityStorage_;
  battle_continuity_storage::ScanResult continuityScan_;
  runtime::ProcessLifecycle lifecycle_;
  BoundedQueue<meta::ClaimCompletion> claimCompletions_;
  BoundedQueue<game_flow::LobbyRoomOutboundIntent> roomOutbounds_;
  BoundedQueue<battle::StateSnapshotProjection> movementSnapshots_;
  BoundedQueue<game_flow::CombatOutboundIntent> combatOutbounds_;
  BoundedQueue<EncodedRudpDatagram> movementDatagrams_;
  std::atomic<bool> fatalStop_{false};
  bool started_{};
  bool recovering_{};
  bool recoveryFailed_{};
  std::uint16_t tcpPort_{};
  std::uint16_t udpPort_{};
  std::uint64_t nextConnectionEpoch_{1u};
  FileDescriptor tcpListener_;
  FileDescriptor udpSocket_;
  std::unique_ptr<runtime::linux::EpollReactor> reactor_;
  std::unique_ptr<runtime::WorkerPool> workers_;
  std::unique_ptr<runtime::ThreadDeadlineScheduler> deadlines_;
  settlement::SettlementCapacityGate capacity_;
  std::unique_ptr<settlement_storage::StorageWorker> storageWorker_;
  std::unique_ptr<observability::GameMetrics> gameMetrics_;
  std::unique_ptr<ObservedSettlementStorage> observedStorage_;
  session::SessionRegistry sessions_;
  transport::rudp::RudpBindingRegistry bindings_;
  std::unique_ptr<RudpGameplayReadiness> readiness_;
  std::unique_ptr<AuthClaimCoordinator> correlations_;
  std::unique_ptr<meta::MetaClaimClient> claimClient_;
  std::unique_ptr<SessionAuthFlow> authFlow_;
  std::unique_ptr<game_flow::RoomCommandGateway> gateway_;
  std::unique_ptr<RudpMovementFlow> movementFlow_;
  std::unique_ptr<RudpCombatFlow> combatFlow_;
  std::unique_ptr<LobbyRoomFlow> lobbyFlow_;
  std::unique_ptr<BattleLoadFlow> battleLoadFlow_;
  std::unique_ptr<meta::MetaSettlementClient> settlementClient_;
  std::unique_ptr<settlement::SettlementPublisher> publisher_;
  std::unique_ptr<SettlementPublisherDriver> publisherDriver_;
  std::unique_ptr<observability::PrivateMetricsServer> metricsServer_;
  std::map<int, ConnectionState> connections_;
  std::map<std::uint64_t, int> epochToDescriptor_;
  std::map<shared::SessionId, std::uint64_t> sessionToEpoch_;
  std::map<shared::RoomId, std::set<shared::SessionId>> roomMembers_;
  std::map<shared::SessionId, shared::RoomId> sessionRoom_;
  std::map<shared::RoomId, ActiveBattleTick> activeBattles_;
  std::map<shared::SessionId, DetachedBattleSession> detachedBattleSessions_;
  std::map<shared::SessionId, PendingResume> pendingResumes_;
  std::map<BattleKey, BattleStateCache> battleStateCache_;
  std::map<BattleKey, bool> pendingBattleRetirements_;
  std::set<shared::SessionId> resumeAwaitingProjection_;
  std::map<shared::SessionId, std::uint64_t> resumeSnapshotAcks_;
  std::uint64_t nextResumeSnapshotId_{1u};
  std::chrono::steady_clock::time_point nextMovementTick_{};
  std::chrono::steady_clock::time_point nextRudpExpiry_{};
};

bool installSignalHandlers() {
  struct sigaction action {};
  action.sa_handler = requestProcessStop;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  return ::sigaction(SIGINT, &action, nullptr) == 0 &&
         ::sigaction(SIGTERM, &action, nullptr) == 0;
}

} // namespace

int runConfiguredGameServer(const std::filesystem::path &configPath) {
  gStopSignal = 0;
  const auto config = loadConfig(configPath);
  if (!config.has_value() || !installSignalHandlers()) {
    std::cerr << "startup failed: invalid config\n";
    return 2;
  }
  const auto journalDirectory = config->journalPath.parent_path();
  if (settlement_storage::probeJournalDirectory(journalDirectory) !=
      settlement_storage::StorageProbeResult::Ready) {
    std::cerr << "startup failed: storage probe\n";
    return 3;
  }
  settlement_storage::SegmentJournal startupProbe{config->journalPath};
  auto recovery = startupProbe.recoverAndRepair();
  if (!recovery.has_value() || !startupProbe.healthy()) {
    std::cerr << "startup failed: journal recovery\n";
    return 3;
  }
  std::unique_ptr<battle_continuity_storage::ContinuityStorage>
      continuityStorage;
  battle_continuity_storage::ScanResult continuityScan;
  if (config->battleContinuityRoot.has_value()) {
    auto opened = battle_continuity_storage::ContinuityStorage::open(
        *config->battleContinuityRoot, *config->battleContinuityKeyFile);
    if (!opened.ok()) {
      std::cerr << "startup failed: battle continuity storage\n";
      return 3;
    }
    continuityStorage = std::move(opened.storage);
    continuityScan = continuityStorage->scan();
    if (!continuityScan.ok()) {
      std::cerr << "startup failed: battle continuity scan\n";
      return 3;
    }
    std::cout << "RECOVERING epoch=" << continuityStorage->writerRecoveryEpoch()
              << " battles=" << continuityScan.healthy.size()
              << " quarantined=" << continuityScan.quarantined.size()
              << " repaired_tails=" << continuityScan.repairedTails.size()
              << '\n'
              << std::flush;
  }
  ConfiguredGameServer server{*config, std::move(*recovery),
                              std::move(continuityStorage),
                              std::move(continuityScan)};
  if (!server.start()) {
    std::cerr << "startup failed: runtime\n";
    return 4;
  }
  return server.run();
}

} // namespace lol::app
