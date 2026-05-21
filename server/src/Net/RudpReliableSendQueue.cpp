#include "Net/RudpReliableSendQueue.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace {
bool isSequenceNewer(uint32_t candidate, uint32_t baseline) {
    return static_cast<int32_t>(candidate - baseline) > 0;
}

bool isAckedBy(uint32_t sequence, uint32_t ack, uint32_t ackBits) {
    if (sequence == ack) {
        return true;
    }
    if (isSequenceNewer(sequence, ack)) {
        return false;
    }

    const uint32_t distance = ack - sequence;
    if (distance == 0 || distance > 32) {
        return false;
    }

    const uint32_t bit = 1U << (distance - 1);
    return (ackBits & bit) != 0;
}
}  // namespace

namespace Net {
bool RudpReliableSendQueue::track(
    uint32_t sequence,
    const std::vector<uint8_t>& packetBytes) {
    return track(sequence, packetBytes, Clock::now());
}

bool RudpReliableSendQueue::track(
    uint32_t sequence,
    const std::vector<uint8_t>& packetBytes,
    TimePoint now) {
    if (contains(sequence)) {
        return false;
    }

    pending_.push_back(PendingPacket{sequence, packetBytes, now, 0});
    return true;
}

size_t RudpReliableSendQueue::consumeAck(uint32_t ack, uint32_t ackBits) {
    const size_t before = pending_.size();
    pending_.erase(
        std::remove_if(
            pending_.begin(),
            pending_.end(),
            [ack, ackBits](const PendingPacket& packet) {
                return isAckedBy(packet.sequence, ack, ackBits);
            }),
        pending_.end());
    return before - pending_.size();
}

std::vector<uint32_t> RudpReliableSendQueue::dueForRetransmission(
    TimePoint now) const {
    std::vector<uint32_t> sequences;
    for (const PendingPacket& packet : pending_) {
        if (packet.retransmissionCount < kDefaultMaxRetransmissions &&
            isTimedOut(packet, now)) {
            sequences.push_back(packet.sequence);
        }
    }
    return sequences;
}

std::vector<uint32_t> RudpReliableSendQueue::expiredSequences(
    TimePoint now) const {
    std::vector<uint32_t> sequences;
    for (const PendingPacket& packet : pending_) {
        if (packet.retransmissionCount >= kDefaultMaxRetransmissions &&
            isTimedOut(packet, now)) {
            sequences.push_back(packet.sequence);
        }
    }
    return sequences;
}

bool RudpReliableSendQueue::markRetransmitted(
    uint32_t sequence,
    TimePoint now) {
    PendingPacket* packet = findPending(sequence);
    if (packet == nullptr) {
        return false;
    }

    ++packet->retransmissionCount;
    packet->lastSentAt = now;
    return true;
}

bool RudpReliableSendQueue::contains(uint32_t sequence) const {
    return packetBytes(sequence) != nullptr;
}

size_t RudpReliableSendQueue::pendingCount() const {
    return pending_.size();
}

std::vector<uint32_t> RudpReliableSendQueue::pendingSequences() const {
    std::vector<uint32_t> sequences;
    sequences.reserve(pending_.size());
    for (const PendingPacket& packet : pending_) {
        sequences.push_back(packet.sequence);
    }
    return sequences;
}

const std::vector<uint8_t>* RudpReliableSendQueue::packetBytes(uint32_t sequence) const {
    const PendingPacket* packet = findPending(sequence);
    if (packet == nullptr) {
        return nullptr;
    }
    return &packet->packetBytes;
}

uint32_t RudpReliableSendQueue::retransmissionCount(uint32_t sequence) const {
    const PendingPacket* packet = findPending(sequence);
    if (packet == nullptr) {
        return 0;
    }
    return packet->retransmissionCount;
}

const RudpReliableSendQueue::TimePoint* RudpReliableSendQueue::lastSentAt(
    uint32_t sequence) const {
    const PendingPacket* packet = findPending(sequence);
    if (packet == nullptr) {
        return nullptr;
    }
    return &packet->lastSentAt;
}

RudpReliableSendQueue::PendingPacket* RudpReliableSendQueue::findPending(
    uint32_t sequence) {
    const auto it = std::find_if(
        pending_.begin(),
        pending_.end(),
        [sequence](const PendingPacket& packet) {
            return packet.sequence == sequence;
        });
    if (it == pending_.end()) {
        return nullptr;
    }
    return &(*it);
}

const RudpReliableSendQueue::PendingPacket* RudpReliableSendQueue::findPending(
    uint32_t sequence) const {
    const auto it = std::find_if(
        pending_.begin(),
        pending_.end(),
        [sequence](const PendingPacket& packet) {
            return packet.sequence == sequence;
        });
    if (it == pending_.end()) {
        return nullptr;
    }
    return &(*it);
}

bool RudpReliableSendQueue::isTimedOut(
    const PendingPacket& packet,
    TimePoint now) const {
    return now - packet.lastSentAt >= kDefaultRetransmissionTimeout;
}
}  // namespace Net
