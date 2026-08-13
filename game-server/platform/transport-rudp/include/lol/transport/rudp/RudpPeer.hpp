#pragma once

#include <cstdint>

namespace lol::transport::rudp {

enum class ReceiveDisposition : std::uint8_t {
  Newest,
  Reordered,
  Duplicate,
  Stale,
  InvalidSequence,
};

struct AckState final {
  std::uint32_t ack;
  std::uint32_t ackBits;

  bool operator==(const AckState &) const = default;
};

class RudpPeerDelivery final {
public:
  [[nodiscard]] ReceiveDisposition observe(std::uint32_t sequence) noexcept;
  [[nodiscard]] AckState ackState() const noexcept;

private:
  std::uint32_t ack_{0};
  std::uint32_t ackBits_{0};
};

[[nodiscard]] bool isAcknowledged(std::uint32_t sequence,
                                  AckState state) noexcept;

} // namespace lol::transport::rudp
