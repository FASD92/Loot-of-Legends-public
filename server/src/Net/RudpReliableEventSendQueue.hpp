#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Net/RudpPacket.hpp"
#include "Net/RudpReliableSendQueue.hpp"

namespace Net {
enum class RudpReliableEventKind {
    kBattleStart,
    kMonsterDeath,
    kLootResolved,
    kMetaResponse,
};

enum class RudpReliableEventTrackResult {
    kTracked,
    kDuplicateSequence,
    kDuplicateLogicalEvent,
    kInvalidDescriptor,
    kInvalidPacketBytes,
};

struct RudpReliableEventDescriptor {
    RudpReliableEventKind kind{RudpReliableEventKind::kBattleStart};
    std::string logicalKey;
    uint16_t packetType{0};
    uint8_t channelId{0};
};

struct RudpReliableEventPendingEntry {
    RudpReliableEventDescriptor descriptor;
    uint32_t sequence{0};
};

class RudpReliableEventSendQueue {
public:
    using TimePoint = RudpReliableSendQueue::TimePoint;

    RudpReliableEventTrackResult track(
        const RudpReliableEventDescriptor& descriptor,
        uint32_t sequence,
        const std::vector<uint8_t>& packetBytes,
        TimePoint now);

    size_t consumeAck(uint32_t ack, uint32_t ackBits);
    bool remove(uint32_t sequence);
    std::vector<uint32_t> dueForRetransmission(TimePoint now) const;
    std::vector<uint32_t> expiredSequences(TimePoint now) const;
    bool markRetransmitted(uint32_t sequence, TimePoint now);

    size_t pendingCount() const;
    bool containsSequence(uint32_t sequence) const;
    bool containsLogicalEvent(
        RudpReliableEventKind kind,
        const std::string& logicalKey) const;
    const RudpReliableEventPendingEntry* pendingEntry(uint32_t sequence) const;
    const std::vector<uint8_t>* packetBytes(uint32_t sequence) const;
    uint32_t retransmissionCount(uint32_t sequence) const;
    const TimePoint* lastSentAt(uint32_t sequence) const;
    std::vector<uint32_t> pendingSequences() const;

private:
    static bool isValidDescriptor(const RudpReliableEventDescriptor& descriptor);

    std::vector<RudpReliableEventPendingEntry>::iterator findPendingEntry(
        uint32_t sequence);
    std::vector<RudpReliableEventPendingEntry>::const_iterator findPendingEntry(
        uint32_t sequence) const;

    RudpReliableSendQueue reliableQueue_;
    std::vector<RudpReliableEventPendingEntry> pendingEntries_;
};
}  // namespace Net
