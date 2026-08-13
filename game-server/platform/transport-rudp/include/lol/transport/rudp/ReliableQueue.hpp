#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
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
};

class ReliableQueue final {
public:
  using Clock = std::chrono::steady_clock;

  [[nodiscard]] ReliableQueueAdmission enqueue(std::uint32_t sequence,
                                               std::vector<std::byte> datagram,
                                               ReliableLane lane,
                                               Clock::time_point now);
  [[nodiscard]] ReliablePollResult poll(Clock::time_point now);
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
    std::chrono::milliseconds retryDelay;
    std::uint8_t transmissions;
  };

  std::vector<Entry> entries_;
  std::size_t applicationEntries_{0};
  std::size_t bytes_{0};
};

} // namespace lol::transport::rudp
