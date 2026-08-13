#include "ConfiguredGameServer.hpp"

#include "AuthClaimCoordinator.hpp"
#include "BattleLoadFlow.hpp"
#include "LobbyRoomFlow.hpp"
#include "RudpCombatFlow.hpp"
#include "RudpMovementFlow.hpp"
#include "SessionAuthFlow.hpp"

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
constexpr auto kMovementTickInterval = 50ms;
constexpr auto kRudpExpiryInterval = 250ms;
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
        "metrics_read_credential_file", "metrics_source_identity_digest"}) {
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

class ConfiguredGameServer final {
public:
  ConfiguredGameServer(ServerConfig config,
                       settlement_storage::JournalRecoveryResult recovery)
      : config_(std::move(config)), recovery_(std::move(recovery)),
        claimCompletions_(kApplicationQueueCapacity),
        roomOutbounds_(kApplicationQueueCapacity),
        movementDatagrams_(kApplicationQueueCapacity) {}

  ~ConfiguredGameServer() { static_cast<void>(stop()); }

  bool start() {
    if (started_) {
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
      gateway_ = std::make_unique<game_flow::RoomCommandGateway>(
          *workers_, *readiness_,
          [this](game_flow::LobbyRoomOutboundIntent intent) {
            if (!roomOutbounds_.push(std::move(intent))) {
              fatalStop_.store(true, std::memory_order_release);
            }
          },
          [this](battle::StateSnapshotProjection snapshot) {
            if (gameMetrics_ != nullptr) {
              gameMetrics_->recordSnapshot(std::chrono::steady_clock::now());
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
                break;
              }
            }
          },
          [this](game_flow::CombatOutboundIntent intent) {
            if (combatFlow_ == nullptr) {
              fatalStop_.store(true, std::memory_order_release);
              return;
            }
            combatFlow_->handleCombatOutbound(std::move(intent));
          },
          *deadlines_, capacity_, *observedStorage_);
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
              return gameMetrics_->snapshot(gateway_->observation(),
                                            capacity_.metrics());
            });
      }

      if (runtime::linux::EpollReactor::supported()) {
        reactor_ = std::make_unique<runtime::linux::EpollReactor>(
            config_.maxConnections + 2u);
        if (!reactor_->valid()) {
          return false;
        }
      }

      auto tcp = bindSocket(SOCK_STREAM, config_.bindAddress, config_.tcpPort,
                            tcpPort_);
      auto udp = bindSocket(SOCK_DGRAM, config_.bindAddress, config_.udpPort,
                            udpPort_);
      if (!tcp.has_value() || !udp.has_value()) {
        return false;
      }
      tcpListener_ = std::move(*tcp);
      udpSocket_ = std::move(*udp);
      if (reactor_ != nullptr && (!reactor_->watch(tcpListener_.get()) ||
                                  !reactor_->watch(udpSocket_.get()))) {
        return false;
      }
      if ((metricsServer_ != nullptr && !metricsServer_->start()) ||
          !publisherDriver_->start() || !lifecycle_.start()) {
        return false;
      }
      if (::listen(tcpListener_.get(), 128) != 0) {
        return false;
      }
      nextMovementTick_ =
          std::chrono::steady_clock::now() + kMovementTickInterval;
      nextRudpExpiry_ = std::chrono::steady_clock::now() + kRudpExpiryInterval;
      started_ = true;
      std::cout << "READY tcp=" << tcpPort_ << " udp=" << udpPort_ << '\n'
                << std::flush;
      return true;
    } catch (...) {
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
    std::vector<std::byte> postAuthInbound;
  };

  struct ActiveBattleTick final {
    shared::BattleInstanceId battleId;
    std::uint32_t nextServerTick{1u};
  };

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
      if (const auto *request =
              std::get_if<transport::tcp::RequestRudpBindCapability>(
                  &*sessionMessage.message)) {
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
      auto frames = authFlow_->complete(completion, unixTimeMilliseconds());
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
      if (authenticated.has_value()) {
        const auto descriptor = epochToDescriptor_.find(authenticatedEpoch);
        if (descriptor != epochToDescriptor_.end()) {
          connections_.at(descriptor->second).session = authenticated;
          sessionToEpoch_.insert_or_assign(authenticated->sessionId,
                                           authenticatedEpoch);
        }
      }
      for (auto &frame : frames) {
        applySessionFrame(std::move(frame));
      }
      if (authenticated.has_value() && !gateway_->enterLobby(*authenticated)) {
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

  void updateRoutingState(const game_flow::LobbyRoomOutboundIntent &intent) {
    if (const auto *entry =
            std::get_if<game_flow::LobbyEntrySnapshot>(&intent.message)) {
      removeSessionFromRoom(entry->session.sessionId);
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
    }
    if (const auto *started =
            std::get_if<game_flow::ArenaGameplayStart>(&intent.message)) {
      activeBattles_.insert_or_assign(
          started->roomId, ActiveBattleTick{.battleId = started->battleId,
                                            .nextServerTick = 1u});
    }
    if (const auto *cancelled =
            std::get_if<game_flow::ArenaLoadCancelled>(&intent.message)) {
      const auto battle = activeBattles_.find(cancelled->roomId);
      if (battle != activeBattles_.end() &&
          battle->second.battleId == cancelled->battleId) {
        activeBattles_.erase(battle);
      }
    }
    if (const auto *result =
            std::get_if<battle::BattleFinalResult>(&intent.message)) {
      const auto active = activeBattles_.find(result->roomId);
      if (active != activeBattles_.end() &&
          active->second.battleId == result->battleId) {
        activeBattles_.erase(active);
      }
    }
    if (const auto *recovery =
            std::get_if<game_flow::BattleRecoveryNotice>(&intent.message)) {
      const auto active = activeBattles_.find(recovery->roomId);
      if (active != activeBattles_.end() &&
          active->second.battleId == recovery->battleId) {
        activeBattles_.erase(active);
      }
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
            if (route != sessionToEpoch_.end()) {
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
              if (!sessionRoom_.contains(sessionId)) {
                epochs.insert(epoch);
              }
            }
          } else {
            const auto room = roomMembers_.find(target.roomId);
            if (room != roomMembers_.end()) {
              for (const auto sessionId : room->second) {
                const auto route = sessionToEpoch_.find(sessionId);
                if (route != sessionToEpoch_.end()) {
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
      sendDatagram(datagram);
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

  void sendDatagram(const EncodedRudpDatagram &datagram) {
    const auto address = socketAddressFor(datagram.endpoint);
    if (!address.has_value()) {
      return;
    }
    const auto sent = ::sendto(
        udpSocket_.get(), datagram.datagram.data(), datagram.datagram.size(), 0,
        reinterpret_cast<const sockaddr *>(&*address), sizeof(*address));
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      fatalStop_.store(true, std::memory_order_release);
    }
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
    if (header.messageId == 24u) {
      const auto received = bindings_.receive(header, endpoint, now);
      if (received.status == transport::rudp::RudpPacketStatus::Current &&
          combatFlow_ != nullptr) {
        static_cast<void>(combatFlow_->discardAcknowledged(
            header.sessionId, header.sessionGeneration, header.transportEpoch,
            header.ack, header.ackBits));
      }
      return;
    }
    if (header.messageId == 25u && movementFlow_ != nullptr) {
      static_cast<void>(movementFlow_->submitMove(datagram, endpoint, now));
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
      static_cast<void>(
          gateway_->disconnect(session.sessionId, session.generation));
      const auto current = sessionToEpoch_.find(session.sessionId);
      if (current != sessionToEpoch_.end() &&
          current->second == connection->second.epoch) {
        removeSessionFromRoom(session.sessionId);
        sessionToEpoch_.erase(current);
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
    const auto members = roomMembers_.find(room->second);
    if (members != roomMembers_.end()) {
      members->second.erase(sessionId);
      if (members->second.empty()) {
        roomMembers_.erase(members);
      }
    }
    sessionRoom_.erase(room);
  }

  ServerConfig config_;
  settlement_storage::JournalRecoveryResult recovery_;
  runtime::ProcessLifecycle lifecycle_;
  BoundedQueue<meta::ClaimCompletion> claimCompletions_;
  BoundedQueue<game_flow::LobbyRoomOutboundIntent> roomOutbounds_;
  BoundedQueue<EncodedRudpDatagram> movementDatagrams_;
  std::atomic<bool> fatalStop_{false};
  bool started_{};
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
  ConfiguredGameServer server{*config, std::move(*recovery)};
  if (!server.start()) {
    std::cerr << "startup failed: runtime\n";
    return 4;
  }
  return server.run();
}

} // namespace lol::app
