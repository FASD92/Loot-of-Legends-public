#include <lol/transport/rudp/ReliableQueue.hpp>

#include <lol/transport/rudp/RudpPeer.hpp>

#include <algorithm>
#include <utility>

namespace lol::transport::rudp {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMaximumEntries = 256;
constexpr std::size_t kMaximumApplicationEntries = 224;
constexpr std::size_t kMaximumBytes = 262144;
constexpr std::size_t kMaximumDatagramBytes = 1200;
constexpr std::uint8_t kMaximumTransmissions = 5;
constexpr auto kInitialRetryDelay = 200ms;
constexpr auto kMaximumRetryDelay = 1000ms;
constexpr auto kExpiry = 5000ms;

} // namespace

ReliableQueueAdmission ReliableQueue::enqueue(std::uint32_t sequence,
                                              std::vector<std::byte> datagram,
                                              ReliableLane lane,
                                              Clock::time_point now) {
  if (sequence == 0) {
    return ReliableQueueAdmission::InvalidSequence;
  }
  if (datagram.empty() || datagram.size() > kMaximumDatagramBytes) {
    return ReliableQueueAdmission::InvalidDatagram;
  }
  if (contains(sequence)) {
    return ReliableQueueAdmission::DuplicateSequence;
  }
  if (lane == ReliableLane::Application &&
      applicationEntries_ >= kMaximumApplicationEntries) {
    return ReliableQueueAdmission::ApplicationLimitReached;
  }
  if (entries_.size() >= kMaximumEntries) {
    return ReliableQueueAdmission::EntryLimitReached;
  }
  if (datagram.size() > kMaximumBytes - bytes_) {
    return ReliableQueueAdmission::ByteLimitReached;
  }

  bytes_ += datagram.size();
  if (lane == ReliableLane::Application) {
    ++applicationEntries_;
  }
  entries_.push_back(Entry{
      .sequence = sequence,
      .lane = lane,
      .datagram = std::move(datagram),
      .queuedAt = now,
      .nextTransmissionAt = now,
      .retryDelay = kInitialRetryDelay,
      .transmissions = 0,
  });
  return ReliableQueueAdmission::Accepted;
}

ReliablePollResult ReliableQueue::poll(Clock::time_point now) {
  ReliablePollResult result;
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    if (now - entry->queuedAt >= kExpiry) {
      result.expiredSequences.push_back(entry->sequence);
      bytes_ -= entry->datagram.size();
      if (entry->lane == ReliableLane::Application) {
        --applicationEntries_;
      }
      entry = entries_.erase(entry);
      continue;
    }
    if (entry->transmissions < kMaximumTransmissions &&
        now >= entry->nextTransmissionAt) {
      ++entry->transmissions;
      result.transmissions.push_back(ReliableTransmission{
          .sequence = entry->sequence,
          .attempt = entry->transmissions,
          .datagram = entry->datagram,
      });
      if (entry->transmissions < kMaximumTransmissions) {
        entry->nextTransmissionAt = now + entry->retryDelay;
        entry->retryDelay = std::min(entry->retryDelay * 2, kMaximumRetryDelay);
      }
    }
    ++entry;
  }
  return result;
}

std::size_t ReliableQueue::discardAcknowledged(std::uint32_t ack,
                                               std::uint32_t ackBits) {
  std::size_t removed = 0;
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    if (entry->transmissions == 0 ||
        !isAcknowledged(entry->sequence,
                        AckState{.ack = ack, .ackBits = ackBits})) {
      ++entry;
      continue;
    }
    bytes_ -= entry->datagram.size();
    if (entry->lane == ReliableLane::Application) {
      --applicationEntries_;
    }
    entry = entries_.erase(entry);
    ++removed;
  }
  return removed;
}

bool ReliableQueue::contains(std::uint32_t sequence) const noexcept {
  return std::ranges::any_of(entries_, [sequence](const Entry &entry) {
    return entry.sequence == sequence;
  });
}

bool ReliableQueue::empty() const noexcept { return entries_.empty(); }

std::size_t ReliableQueue::size() const noexcept { return entries_.size(); }

std::size_t ReliableQueue::applicationSize() const noexcept {
  return applicationEntries_;
}

std::size_t ReliableQueue::byteSize() const noexcept { return bytes_; }

} // namespace lol::transport::rudp
