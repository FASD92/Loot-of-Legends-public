#include <lol/transport/rudp/ReliableQueue.hpp>
#include <lol/transport/rudp/RudpPeer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::transport::rudp::AckState;
using lol::transport::rudp::ReceiveDisposition;
using lol::transport::rudp::ReliableLane;
using lol::transport::rudp::ReliableQueue;
using lol::transport::rudp::ReliableQueueAdmission;
using lol::transport::rudp::RudpPeerDelivery;

constexpr auto kStart = std::chrono::steady_clock::time_point{};

std::vector<std::byte> datagram(std::size_t bytes = 48) {
  return std::vector<std::byte>(bytes, std::byte{0});
}

bool receiveWindowTracksReorderDuplicateAndWrap() {
  RudpPeerDelivery delivery;
  if (delivery.observe(10) != ReceiveDisposition::Newest ||
      delivery.ackState() != AckState{.ack = 10, .ackBits = 0} ||
      delivery.observe(8) != ReceiveDisposition::Reordered ||
      delivery.ackState() != AckState{.ack = 10, .ackBits = 0b10} ||
      delivery.observe(9) != ReceiveDisposition::Reordered ||
      delivery.ackState() != AckState{.ack = 10, .ackBits = 0b11} ||
      delivery.observe(8) != ReceiveDisposition::Duplicate ||
      delivery.observe(11) != ReceiveDisposition::Newest ||
      delivery.ackState() != AckState{.ack = 11, .ackBits = 0b111}) {
    return false;
  }

  RudpPeerDelivery stale;
  if (stale.observe(100) != ReceiveDisposition::Newest ||
      stale.observe(67) != ReceiveDisposition::Stale ||
      stale.observe(0) != ReceiveDisposition::InvalidSequence) {
    return false;
  }

  RudpPeerDelivery wrapping;
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  return wrapping.observe(maximum) == ReceiveDisposition::Newest &&
         wrapping.observe(1) == ReceiveDisposition::Newest &&
         wrapping.ackState() == AckState{.ack = 1, .ackBits = 0b10} &&
         wrapping.observe(maximum) == ReceiveDisposition::Duplicate;
}

bool retransmissionUsesFrozenBackoffAndExpiry() {
  ReliableQueue queue;
  if (queue.enqueue(1, datagram(), ReliableLane::Application, kStart) !=
      ReliableQueueAdmission::Accepted) {
    return false;
  }

  const auto first = queue.poll(kStart);
  const auto earlySecond = queue.poll(kStart + 199ms);
  const auto second = queue.poll(kStart + 200ms);
  const auto earlyThird = queue.poll(kStart + 599ms);
  const auto third = queue.poll(kStart + 600ms);
  const auto fourth = queue.poll(kStart + 1400ms);
  const auto fifth = queue.poll(kStart + 2400ms);
  const auto exhausted = queue.poll(kStart + 4999ms);
  const auto expired = queue.poll(kStart + 5000ms);

  const auto isAttempt = [](const auto &poll, std::uint8_t attempt) {
    return poll.transmissions.size() == 1 &&
           poll.transmissions.front().sequence == 1 &&
           poll.transmissions.front().attempt == attempt &&
           poll.expiredSequences.empty();
  };
  return isAttempt(first, 1) && earlySecond.transmissions.empty() &&
         isAttempt(second, 2) && earlyThird.transmissions.empty() &&
         isAttempt(third, 3) && isAttempt(fourth, 4) && isAttempt(fifth, 5) &&
         exhausted.transmissions.empty() && queue.empty() &&
         expired.transmissions.empty() &&
         expired.expiredSequences == std::vector<std::uint32_t>{1};
}

bool acknowledgementOnlyDiscardsTransportEntries() {
  ReliableQueue queue;
  for (const std::uint32_t sequence : {69U, 98U, 99U, 100U}) {
    if (queue.enqueue(sequence, datagram(), ReliableLane::Application,
                      kStart) != ReliableQueueAdmission::Accepted) {
      return false;
    }
  }
  if (queue.poll(kStart).transmissions.size() != 4 ||
      queue.enqueue(101, datagram(), ReliableLane::Application, kStart) !=
          ReliableQueueAdmission::Accepted) {
    return false;
  }
  const std::size_t removed =
      queue.discardAcknowledged(101, (1U << 0U) | (1U << 1U) | (1U << 31U));
  return removed == 3 && queue.size() == 2 && queue.contains(98) &&
         queue.contains(101) && !queue.contains(69) && !queue.contains(99) &&
         !queue.contains(100);
}

bool queuePreservesControlReserveAndByteBound() {
  ReliableQueue queue;
  for (std::uint32_t sequence = 1; sequence <= 224; ++sequence) {
    if (queue.enqueue(sequence, datagram(1), ReliableLane::Application,
                      kStart) != ReliableQueueAdmission::Accepted) {
      return false;
    }
  }
  if (queue.enqueue(225, datagram(1), ReliableLane::Application, kStart) !=
      ReliableQueueAdmission::ApplicationLimitReached) {
    return false;
  }
  for (std::uint32_t sequence = 225; sequence <= 256; ++sequence) {
    if (queue.enqueue(sequence, datagram(1), ReliableLane::Control, kStart) !=
        ReliableQueueAdmission::Accepted) {
      return false;
    }
  }
  if (queue.enqueue(257, datagram(1), ReliableLane::Control, kStart) !=
          ReliableQueueAdmission::EntryLimitReached ||
      queue.size() != 256 || queue.applicationSize() != 224 ||
      queue.byteSize() != 256) {
    return false;
  }

  ReliableQueue byteBound;
  for (std::uint32_t sequence = 1; sequence <= 218; ++sequence) {
    if (byteBound.enqueue(sequence, datagram(1200), ReliableLane::Application,
                          kStart) != ReliableQueueAdmission::Accepted) {
      return false;
    }
  }
  return byteBound.byteSize() == 261600 &&
         byteBound.enqueue(219, datagram(1200), ReliableLane::Application,
                           kStart) ==
             ReliableQueueAdmission::ByteLimitReached &&
         byteBound.enqueue(1, datagram(), ReliableLane::Control, kStart) ==
             ReliableQueueAdmission::DuplicateSequence &&
         byteBound.enqueue(300, {}, ReliableLane::Control, kStart) ==
             ReliableQueueAdmission::InvalidDatagram &&
         byteBound.enqueue(300, datagram(1201), ReliableLane::Control,
                           kStart) == ReliableQueueAdmission::InvalidDatagram;
}

} // namespace

int main() {
  return receiveWindowTracksReorderDuplicateAndWrap() &&
                 retransmissionUsesFrozenBackoffAndExpiry() &&
                 acknowledgementOnlyDiscardsTransportEntries() &&
                 queuePreservesControlReserveAndByteBound()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
