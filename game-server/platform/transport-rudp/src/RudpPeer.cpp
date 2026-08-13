#include <lol/transport/rudp/RudpPeer.hpp>

#include <lol/transport/rudp/RudpHeader.hpp>

namespace lol::transport::rudp {

ReceiveDisposition RudpPeerDelivery::observe(std::uint32_t sequence) noexcept {
  if (sequence == 0) {
    return ReceiveDisposition::InvalidSequence;
  }
  if (ack_ == 0) {
    ack_ = sequence;
    return ReceiveDisposition::Newest;
  }
  if (sequence == ack_) {
    return ReceiveDisposition::Duplicate;
  }
  if (isSequenceNewer(sequence, ack_)) {
    const std::uint32_t distance = sequence - ack_;
    if (distance > 32) {
      ackBits_ = 0;
    } else {
      const std::uint32_t shifted = distance == 32 ? 0 : ackBits_ << distance;
      ackBits_ = shifted | (1U << (distance - 1U));
    }
    ack_ = sequence;
    return ReceiveDisposition::Newest;
  }

  const std::uint32_t distance = ack_ - sequence;
  if (distance > 32) {
    return ReceiveDisposition::Stale;
  }
  const std::uint32_t bit = 1U << (distance - 1U);
  if ((ackBits_ & bit) != 0U) {
    return ReceiveDisposition::Duplicate;
  }
  ackBits_ |= bit;
  return ReceiveDisposition::Reordered;
}

AckState RudpPeerDelivery::ackState() const noexcept {
  return {.ack = ack_, .ackBits = ackBits_};
}

bool isAcknowledged(std::uint32_t sequence, AckState state) noexcept {
  if (sequence == 0 || state.ack == 0) {
    return false;
  }
  if (sequence == state.ack) {
    return true;
  }
  if (isSequenceNewer(sequence, state.ack)) {
    return false;
  }
  const std::uint32_t distance = state.ack - sequence;
  return distance >= 1 && distance <= 32 &&
         (state.ackBits & (1U << (distance - 1U))) != 0U;
}

} // namespace lol::transport::rudp
