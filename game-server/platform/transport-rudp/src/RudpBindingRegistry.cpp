#include <lol/transport/rudp/RudpBindingRegistry.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <span>
#include <utility>

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#endif

namespace lol::transport::rudp {
namespace {

constexpr std::size_t kEntropyAttempts = 4;
constexpr auto kCapabilityTtl = std::chrono::seconds{15};
constexpr auto kConfirmedPeerTimeout = std::chrono::seconds{5};

bool fillSecureRandom(std::span<std::byte> output) noexcept {
#if defined(__linux__)
  std::size_t filled = 0;
  while (filled < output.size()) {
    const auto received =
        ::getrandom(output.data() + filled, output.size() - filled, 0);
    if (received > 0) {
      filled += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
#elif defined(__APPLE__)
  ::arc4random_buf(output.data(), output.size());
  return true;
#else
  static_cast<void>(output);
  return false;
#endif
}

std::optional<std::uint32_t>
randomEpoch(std::optional<std::uint32_t> previous) noexcept {
  for (std::size_t attempt = 0; attempt < kEntropyAttempts; ++attempt) {
    std::array<std::byte, sizeof(std::uint32_t)> bytes{};
    if (!fillSecureRandom(bytes)) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data(), bytes.size());
    if (value != 0 && value != previous) {
      return value;
    }
  }
  return std::nullopt;
}

RudpBindResult rejected(RudpBindStatus status) {
  return {.status = status,
          .transportEpoch = 0,
          .sequence = 0,
          .ack = AckState{.ack = 0, .ackBits = 0}};
}

bool nonzero(const RudpBindCapability &capability) {
  return std::ranges::any_of(
      capability.bytes, [](std::byte value) { return value != std::byte{0}; });
}

bool sameCapability(const RudpBindCapability &left,
                    const RudpBindCapability &right) {
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < left.bytes.size(); ++index) {
    difference |=
        std::to_integer<std::uint8_t>(left.bytes[index] ^ right.bytes[index]);
  }
  return difference == 0;
}

} // namespace

std::optional<RudpBindCapability>
RudpBindingRegistry::requestCapability(std::uint64_t sessionId,
                                       std::uint64_t sessionGeneration,
                                       Clock::time_point now) {
  if (sessionId == 0 || sessionGeneration == 0) {
    return std::nullopt;
  }
  for (std::size_t attempt = 0; attempt < kEntropyAttempts; ++attempt) {
    RudpBindCapability capability{};
    if (!fillSecureRandom(capability.bytes)) {
      return std::nullopt;
    }
    if (nonzero(capability)) {
      std::lock_guard lock{mutex_};
      pendingCapabilities_.insert_or_assign(
          sessionId, PendingCapability{.sessionGeneration = sessionGeneration,
                                       .capability = capability,
                                       .expiresAt = now + kCapabilityTtl});
      return capability;
    }
  }
  return std::nullopt;
}

RudpBindResult RudpBindingRegistry::bind(const RudpHeader &header,
                                         const RudpBindHello &hello,
                                         RudpEndpoint endpoint,
                                         Clock::time_point now) {
  if (header.flag != RudpFlag::Reliable || header.messageId != 22 ||
      header.transportEpoch != 0 || header.sessionId == 0 ||
      header.sessionGeneration == 0 || header.sequence == 0 ||
      endpoint.port == 0) {
    return rejected(RudpBindStatus::InvalidHello);
  }
  std::lock_guard lock{mutex_};
  const auto accepted = bindings_.find(header.sessionId);
  if (accepted != bindings_.end() &&
      accepted->second.sessionGeneration == header.sessionGeneration &&
      accepted->second.endpoint == endpoint &&
      accepted->second.acceptedHelloSequence == header.sequence &&
      sameCapability(accepted->second.acceptedCapability, hello.capability)) {
    accepted->second.lastSeen = now;
    return {.status = RudpBindStatus::Accepted,
            .transportEpoch = accepted->second.transportEpoch,
            .sequence = 1,
            .ack = accepted->second.acceptedAck};
  }
  const auto pending = pendingCapabilities_.find(header.sessionId);
  if (pending == pendingCapabilities_.end()) {
    return rejected(RudpBindStatus::CapabilityMissing);
  }
  if (pending->second.sessionGeneration != header.sessionGeneration) {
    return rejected(RudpBindStatus::WrongGeneration);
  }
  if (now >= pending->second.expiresAt) {
    pendingCapabilities_.erase(pending);
    return rejected(RudpBindStatus::CapabilityExpired);
  }
  if (!sameCapability(pending->second.capability, hello.capability)) {
    return rejected(RudpBindStatus::CapabilityMismatch);
  }
  pendingCapabilities_.erase(pending);

  const auto existing = bindings_.find(header.sessionId);
  const auto epoch =
      randomEpoch(existing == bindings_.end()
                      ? std::nullopt
                      : std::optional{existing->second.transportEpoch});
  if (!epoch.has_value()) {
    return rejected(RudpBindStatus::EntropyUnavailable);
  }
  RudpPeerDelivery delivery;
  if (delivery.observe(header.sequence) != ReceiveDisposition::Newest) {
    return rejected(RudpBindStatus::InvalidHello);
  }
  const AckState ack = delivery.ackState();
  bindings_.insert_or_assign(
      header.sessionId, Binding{.sessionGeneration = header.sessionGeneration,
                                .transportEpoch = *epoch,
                                .nextOutboundSequence = 2,
                                .endpoint = std::move(endpoint),
                                .acceptedCapability = hello.capability,
                                .acceptedHelloSequence = header.sequence,
                                .acceptedAck = ack,
                                .delivery = std::move(delivery),
                                .lastSeen = now});
  return {.status = RudpBindStatus::Accepted,
          .transportEpoch = *epoch,
          .sequence = 1,
          .ack = ack};
}

RudpPacketStatus RudpBindingRegistry::status(
    std::uint64_t sessionId, std::uint64_t sessionGeneration,
    std::uint32_t transportEpoch, const RudpEndpoint &endpoint) const noexcept {
  std::lock_guard lock{mutex_};
  return statusLocked(sessionId, sessionGeneration, transportEpoch, endpoint);
}

RudpPacketStatus RudpBindingRegistry::statusLocked(
    std::uint64_t sessionId, std::uint64_t sessionGeneration,
    std::uint32_t transportEpoch, const RudpEndpoint &endpoint) const noexcept {
  const auto binding = bindings_.find(sessionId);
  if (binding == bindings_.end()) {
    return RudpPacketStatus::NotBound;
  }
  if (binding->second.sessionGeneration != sessionGeneration) {
    return RudpPacketStatus::WrongGeneration;
  }
  if (binding->second.transportEpoch != transportEpoch) {
    return RudpPacketStatus::StaleEpoch;
  }
  if (binding->second.endpoint != endpoint) {
    return RudpPacketStatus::WrongEndpoint;
  }
  return RudpPacketStatus::Current;
}

RudpReceiveResult RudpBindingRegistry::receive(const RudpHeader &header,
                                               const RudpEndpoint &endpoint,
                                               Clock::time_point now) {
  std::lock_guard lock{mutex_};
  const auto packetStatus =
      statusLocked(header.sessionId, header.sessionGeneration,
                   header.transportEpoch, endpoint);
  if (packetStatus != RudpPacketStatus::Current) {
    return {.status = packetStatus,
            .disposition = std::nullopt,
            .ack = AckState{.ack = 0, .ackBits = 0}};
  }
  auto &binding = bindings_.at(header.sessionId);
  binding.lastSeen = now;
  const auto disposition = binding.delivery.observe(header.sequence);
  return {.status = RudpPacketStatus::Current,
          .disposition = disposition,
          .ack = binding.delivery.ackState()};
}

std::vector<ExpiredRudpBinding>
RudpBindingRegistry::expireTimedOut(Clock::time_point now) {
  std::lock_guard lock{mutex_};
  std::vector<ExpiredRudpBinding> expired;
  for (auto binding = bindings_.begin(); binding != bindings_.end();) {
    if (now < binding->second.lastSeen + kConfirmedPeerTimeout) {
      ++binding;
      continue;
    }
    expired.push_back(ExpiredRudpBinding{
        .sessionId = binding->first,
        .sessionGeneration = binding->second.sessionGeneration,
    });
    binding = bindings_.erase(binding);
  }
  return expired;
}

std::optional<RudpOutboundRoute>
RudpBindingRegistry::nextOutbound(std::uint64_t sessionId) {
  std::lock_guard lock{mutex_};
  const auto binding = bindings_.find(sessionId);
  if (binding == bindings_.end()) {
    return std::nullopt;
  }
  const auto sequence = binding->second.nextOutboundSequence;
  ++binding->second.nextOutboundSequence;
  if (binding->second.nextOutboundSequence == 0) {
    binding->second.nextOutboundSequence = 1;
  }
  return RudpOutboundRoute{
      .sessionId = sessionId,
      .sessionGeneration = binding->second.sessionGeneration,
      .transportEpoch = binding->second.transportEpoch,
      .sequence = sequence,
      .ack = binding->second.delivery.ackState(),
      .endpoint = binding->second.endpoint,
  };
}

bool RudpBindingRegistry::invalidate(std::uint64_t sessionId,
                                     std::uint64_t sessionGeneration) {
  std::lock_guard lock{mutex_};
  bool changed = false;
  const auto pending = pendingCapabilities_.find(sessionId);
  if (pending != pendingCapabilities_.end() &&
      pending->second.sessionGeneration == sessionGeneration) {
    pendingCapabilities_.erase(pending);
    changed = true;
  }
  const auto binding = bindings_.find(sessionId);
  if (binding != bindings_.end() &&
      binding->second.sessionGeneration == sessionGeneration) {
    bindings_.erase(binding);
    changed = true;
  }
  return changed;
}

std::size_t RudpBindingRegistry::boundCount() const noexcept {
  std::lock_guard lock{mutex_};
  return bindings_.size();
}

bool RudpBindingRegistry::isBound(
    std::uint64_t sessionId, std::uint64_t sessionGeneration) const noexcept {
  std::lock_guard lock{mutex_};
  const auto binding = bindings_.find(sessionId);
  return binding != bindings_.end() &&
         binding->second.sessionGeneration == sessionGeneration;
}

} // namespace lol::transport::rudp
