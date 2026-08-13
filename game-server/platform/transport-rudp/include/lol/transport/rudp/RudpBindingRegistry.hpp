#pragma once

#include <lol/transport/rudp/RudpCodec.hpp>
#include <lol/transport/rudp/RudpPeer.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace lol::transport::rudp {

struct RudpEndpoint final {
  std::array<std::byte, 16> address;
  std::uint16_t port;
  std::uint32_t scopeId;

  bool operator==(const RudpEndpoint &) const = default;
};

enum class RudpBindStatus : std::uint8_t {
  Accepted,
  InvalidHello,
  CapabilityMissing,
  CapabilityExpired,
  CapabilityMismatch,
  WrongGeneration,
  EntropyUnavailable,
};

struct RudpBindResult final {
  RudpBindStatus status;
  std::uint32_t transportEpoch;
  std::uint32_t sequence;
  AckState ack;
};

enum class RudpPacketStatus : std::uint8_t {
  Current,
  NotBound,
  WrongGeneration,
  StaleEpoch,
  WrongEndpoint,
};

struct RudpReceiveResult final {
  RudpPacketStatus status;
  std::optional<ReceiveDisposition> disposition;
  AckState ack;
};

struct RudpOutboundRoute final {
  std::uint64_t sessionId;
  std::uint64_t sessionGeneration;
  std::uint32_t transportEpoch;
  std::uint32_t sequence;
  AckState ack;
  RudpEndpoint endpoint;
};

struct ExpiredRudpBinding final {
  std::uint64_t sessionId;
  std::uint64_t sessionGeneration;
};

class RudpBindingRegistry final {
public:
  using Clock = std::chrono::steady_clock;

  [[nodiscard]] std::optional<RudpBindCapability>
  requestCapability(std::uint64_t sessionId, std::uint64_t sessionGeneration,
                    Clock::time_point now);
  [[nodiscard]] RudpBindResult bind(const RudpHeader &header,
                                    const RudpBindHello &hello,
                                    RudpEndpoint endpoint,
                                    Clock::time_point now);
  [[nodiscard]] RudpPacketStatus
  status(std::uint64_t sessionId, std::uint64_t sessionGeneration,
         std::uint32_t transportEpoch,
         const RudpEndpoint &endpoint) const noexcept;
  [[nodiscard]] RudpReceiveResult receive(const RudpHeader &header,
                                          const RudpEndpoint &endpoint,
                                          Clock::time_point now);
  [[nodiscard]] std::vector<ExpiredRudpBinding>
  expireTimedOut(Clock::time_point now);
  [[nodiscard]] std::optional<RudpOutboundRoute>
  nextOutbound(std::uint64_t sessionId);
  [[nodiscard]] bool invalidate(std::uint64_t sessionId,
                                std::uint64_t sessionGeneration);
  [[nodiscard]] std::size_t boundCount() const noexcept;
  [[nodiscard]] bool isBound(std::uint64_t sessionId,
                             std::uint64_t sessionGeneration) const noexcept;

private:
  struct Binding final {
    std::uint64_t sessionGeneration;
    std::uint32_t transportEpoch;
    std::uint32_t nextOutboundSequence;
    RudpEndpoint endpoint;
    RudpBindCapability acceptedCapability;
    std::uint32_t acceptedHelloSequence;
    AckState acceptedAck;
    RudpPeerDelivery delivery;
    Clock::time_point lastSeen;
  };

  struct PendingCapability final {
    std::uint64_t sessionGeneration;
    RudpBindCapability capability;
    Clock::time_point expiresAt;
  };

  [[nodiscard]] RudpPacketStatus
  statusLocked(std::uint64_t sessionId, std::uint64_t sessionGeneration,
               std::uint32_t transportEpoch,
               const RudpEndpoint &endpoint) const noexcept;

  mutable std::mutex mutex_;
  std::map<std::uint64_t, PendingCapability> pendingCapabilities_;
  std::map<std::uint64_t, Binding> bindings_;
};

} // namespace lol::transport::rudp
