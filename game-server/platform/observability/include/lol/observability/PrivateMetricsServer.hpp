#pragma once

#include <lol/observability/GameMetrics.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lol::observability {

struct PrivateMetricsServerConfig final {
  std::string bindAddress;
  std::uint16_t port{};
  std::string readToken;
  std::string sourceIdentityDigest;
  std::chrono::milliseconds requestTimeout;
  std::size_t maxRequestBytes{};
  std::size_t maxResponseBytes{};
};

class PrivateMetricsServer final {
public:
  using SnapshotSource = std::function<GameMetricSnapshot()>;

  PrivateMetricsServer(PrivateMetricsServerConfig config,
                       SnapshotSource source);
  ~PrivateMetricsServer();

  PrivateMetricsServer(const PrivateMetricsServer &) = delete;
  PrivateMetricsServer &operator=(const PrivateMetricsServer &) = delete;

  [[nodiscard]] bool start();
  void stop() noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lol::observability
