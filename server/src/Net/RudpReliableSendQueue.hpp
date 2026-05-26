#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Net {
class RudpReliableSendQueue {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr std::chrono::milliseconds kDefaultRetransmissionTimeout{200};
    static constexpr uint32_t kDefaultMaxRetransmissions{5};

    bool track(uint32_t sequence, const std::vector<uint8_t>& packetBytes);
    bool track(
        uint32_t sequence,
        const std::vector<uint8_t>& packetBytes,
        TimePoint now);
    size_t consumeAck(uint32_t ack, uint32_t ackBits);
    bool remove(uint32_t sequence);
    std::vector<uint32_t> dueForRetransmission(TimePoint now) const;
    std::vector<uint32_t> expiredSequences(TimePoint now) const;
    bool markRetransmitted(uint32_t sequence, TimePoint now);

    bool contains(uint32_t sequence) const;
    size_t pendingCount() const;
    std::vector<uint32_t> pendingSequences() const;
    const std::vector<uint8_t>* packetBytes(uint32_t sequence) const;
    uint32_t retransmissionCount(uint32_t sequence) const;
    const TimePoint* lastSentAt(uint32_t sequence) const;

private:
    struct PendingPacket {
        uint32_t sequence{0};
        std::vector<uint8_t> packetBytes;
        TimePoint lastSentAt{};
        uint32_t retransmissionCount{0};
    };

    PendingPacket* findPending(uint32_t sequence);
    const PendingPacket* findPending(uint32_t sequence) const;
    bool isTimedOut(const PendingPacket& packet, TimePoint now) const;

    std::vector<PendingPacket> pending_;
};
}  // namespace Net
