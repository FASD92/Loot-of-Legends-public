#pragma once

#include "AuthClaimCoordinator.hpp"

#include <lol/meta/MetaClaimClient.hpp>
#include <lol/transport/tcp/SessionProtocolCodec.hpp>
#include <lol/transport/tcp/TcpConnection.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lol::app {

enum class ConnectionTransition {
  KeepAuthenticated,
  MarkAuthenticated,
  CloseAfterWrite,
};

struct RoutedSessionFrame final {
  std::uint64_t connectionEpoch;
  std::vector<std::byte> frame;
  ConnectionTransition transition;
  transport::tcp::CloseReason closeReason;
};

class SessionAuthFlow final {
public:
  SessionAuthFlow(AuthClaimCoordinator &correlations,
                  meta::MetaClaimClient &metaClient) noexcept;

  [[nodiscard]] std::vector<RoutedSessionFrame>
  begin(std::uint64_t connectionEpoch,
        const transport::tcp::NormalizedAuthRequest &request);
  [[nodiscard]] std::vector<RoutedSessionFrame>
  complete(const meta::ClaimCompletion &completion,
           std::uint64_t serverTimeUnixMillis);
  [[nodiscard]] std::optional<RoutedSessionFrame> requestRudpBindCapability(
      std::uint64_t connectionEpoch,
      const transport::tcp::RequestRudpBindCapability &request,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool disconnect(std::uint64_t connectionEpoch);

private:
  AuthClaimCoordinator &correlations_;
  meta::MetaClaimClient &metaClient_;
};

} // namespace lol::app
