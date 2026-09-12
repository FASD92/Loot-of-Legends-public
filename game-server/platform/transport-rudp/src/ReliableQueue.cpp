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
constexpr auto kExpiry = RttEstimator::kExpiry;

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
      .lastTransmissionAt = now,
      .firstSuccessfulSend = std::nullopt,
      .lastSuccessfulSend = std::nullopt,
      .retryDelay = RttEstimator::kBootstrapRto,
      .transmissions = 0,
      .recordedAttempt = 0,
  });
  return ReliableQueueAdmission::Accepted;
}

ReliablePollResult ReliableQueue::poll(Clock::time_point now,
                                       std::chrono::milliseconds initialRto,
                                       std::uint64_t revision) {
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
      if (entry->transmissions == 0) {
        entry->revision = revision;
        entry->retryDelay = std::clamp(initialRto, RttEstimator::kMinimumRto,
                                       RttEstimator::kMaximumRto);
      } else if (entry->recordedAttempt == entry->transmissions &&
                 entry->lastSuccessfulSend &&
                 *entry->lastSuccessfulSend >= entry->lastTransmissionAt) {
        result.timeouts.push_back(
            {entry->revision,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 entry->nextTransmissionAt - entry->lastTransmissionAt)});
      }
      entry->lastTransmissionAt = now;
      ++entry->transmissions;
      result.transmissions.push_back(ReliableTransmission{
          .sequence = entry->sequence,
          .attempt = entry->transmissions,
          .datagram = entry->datagram,
      });
      if (entry->transmissions < kMaximumTransmissions) {
        entry->nextTransmissionAt = now + entry->retryDelay;
        entry->retryDelay =
            std::min(entry->retryDelay * 2, RttEstimator::kMaximumRto);
      }
    }
    ++entry;
  }
  return result;
}

std::size_t ReliableQueue::discardAcknowledged(std::uint32_t ack,
                                               std::uint32_t ackBits) {
  return removeAcknowledged(ack, ackBits, std::nullopt).removed;
}

ReliableAckResult ReliableQueue::acknowledge(std::uint32_t ack,
                                             std::uint32_t ackBits,
                                             Clock::time_point receivedAt) {
  return removeAcknowledged(ack, ackBits, receivedAt);
}

ReliableAckResult
ReliableQueue::removeAcknowledged(std::uint32_t ack, std::uint32_t ackBits,
                                  std::optional<Clock::time_point> receivedAt) {
  ReliableAckResult result;
  std::optional<Clock::time_point> selectedSentAt;
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    if (entry->transmissions == 0 ||
        !isAcknowledged(entry->sequence,
                        AckState{.ack = ack, .ackBits = ackBits})) {
      ++entry;
      continue;
    }
    if (receivedAt) {
      if (*receivedAt - entry->queuedAt >= kExpiry) {
        ++result.expired;
      } else if (entry->transmissions != 1) {
        ++result.retransmitted;
      } else if (!entry->firstSuccessfulSend) {
        ++result.sendUnconfirmed;
      } else {
        const auto sample =
            std::chrono::duration_cast<std::chrono::microseconds>(
                *receivedAt - *entry->firstSuccessfulSend);
        if (sample <= std::chrono::microseconds::zero()) {
          ++result.nonpositive;
        } else {
          if (selectedSentAt)
            ++result.coalesced;
          if (!selectedSentAt ||
              *entry->firstSuccessfulSend > *selectedSentAt) {
            selectedSentAt = entry->firstSuccessfulSend;
            result.sample = sample;
            result.sampleRevision = entry->revision;
          }
        }
      }
    }
    bytes_ -= entry->datagram.size();
    if (entry->lane == ReliableLane::Application) {
      --applicationEntries_;
    }
    entry = entries_.erase(entry);
    ++result.removed;
  }
  return result;
}

bool ReliableQueue::recordSend(std::uint32_t sequence, std::uint8_t attempt,
                               Clock::time_point sentAt, bool succeeded) {
  const auto entry = std::ranges::find(entries_, sequence, &Entry::sequence);
  if (entry == entries_.end() || attempt == 0 ||
      entry->transmissions != attempt || entry->recordedAttempt == attempt ||
      sentAt < entry->lastTransmissionAt || sentAt - entry->queuedAt >= kExpiry)
    return false;
  entry->recordedAttempt = attempt;
  if (succeeded) {
    if (attempt == 1)
      entry->firstSuccessfulSend = sentAt;
    entry->lastSuccessfulSend = sentAt;
  }
  return true;
}

std::vector<std::chrono::milliseconds> ReliableQueue::effectiveRtos() const {
  std::vector<std::chrono::milliseconds> result;
  for (const auto &entry : entries_) {
    if (entry.transmissions > 0 &&
        entry.transmissions < kMaximumTransmissions) {
      result.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
          entry.nextTransmissionAt - entry.lastTransmissionAt));
    }
  }
  return result;
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
