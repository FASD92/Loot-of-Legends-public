#pragma once

#include <lol/transport/rudp/RttEstimator.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lol::transport::rudp {

enum class ReliableLane : std::uint8_t {
  Application,
  Control,
};

enum class ReliableQueueAdmission : std::uint8_t {
  Accepted,
  InvalidSequence,
  InvalidDatagram,
  DuplicateSequence,
  ApplicationLimitReached,
  EntryLimitReached,
  ByteLimitReached,
};

struct ReliableTransmission final {
  std::uint32_t sequence;
  std::uint8_t attempt;
  std::vector<std::byte> datagram;
};

struct ReliablePollResult final {
  std::vector<ReliableTransmission> transmissions;
  std::vector<std::uint32_t> expiredSequences;
  struct Timeout final {
    std::uint64_t revision;
    std::chrono::milliseconds interval;
  };
  std::vector<Timeout> timeouts;
};

struct ReliableAckResult final {
  std::size_t removed{0};
  std::optional<std::chrono::microseconds> sample;
  std::uint64_t sampleRevision{0};
  std::size_t retransmitted{0};
  std::size_t sendUnconfirmed{0};
  std::size_t nonpositive{0};
  std::size_t expired{0};
  std::size_t coalesced{0};
};

class ReliableQueue final {
public:
  using Clock = std::chrono::steady_clock;

  [[nodiscard]] ReliableQueueAdmission enqueue(std::uint32_t sequence,
                                               std::vector<std::byte> datagram,
                                               ReliableLane lane,
                                               Clock::time_point now);
  [[nodiscard]] ReliablePollResult
  poll(Clock::time_point now,
       std::chrono::milliseconds initialRto = RttEstimator::kBootstrapRto,
       std::uint64_t revision = 0);
  [[nodiscard]] bool recordSend(std::uint32_t sequence, std::uint8_t attempt,
                                Clock::time_point sentAt, bool succeeded);
  [[nodiscard]] ReliableAckResult acknowledge(std::uint32_t ack,
                                              std::uint32_t ackBits,
                                              Clock::time_point receivedAt);
  [[nodiscard]] std::vector<std::chrono::milliseconds> effectiveRtos() const;
  [[nodiscard]] std::size_t discardAcknowledged(std::uint32_t ack,
                                                std::uint32_t ackBits);

  [[nodiscard]] bool contains(std::uint32_t sequence) const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t applicationSize() const noexcept;
  [[nodiscard]] std::size_t byteSize() const noexcept;

private:
  struct Entry final {
    std::uint32_t sequence;
    ReliableLane lane;
    std::vector<std::byte> datagram;
    Clock::time_point queuedAt;
    Clock::time_point nextTransmissionAt;
    Clock::time_point lastTransmissionAt;
    std::optional<Clock::time_point> firstSuccessfulSend;
    std::optional<Clock::time_point> lastSuccessfulSend;
    std::chrono::milliseconds retryDelay;
    std::uint8_t transmissions;
    std::uint8_t recordedAttempt;
    std::uint64_t revision{0};
  };

  [[nodiscard]] ReliableAckResult
  removeAcknowledged(std::uint32_t ack, std::uint32_t ackBits,
                     std::optional<Clock::time_point> receivedAt);

  std::vector<Entry> entries_;
  std::size_t applicationEntries_{0};
  std::size_t bytes_{0};
};

} // namespace lol::transport::rudp
