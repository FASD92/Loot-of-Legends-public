#include "Net/RudpReliableEventSendQueue.hpp"

#include <algorithm>

namespace {
bool expectedPacketContract(
    Net::RudpReliableEventKind kind,
    uint16_t& outPacketType,
    uint8_t& outChannelId) {
    switch (kind) {
        case Net::RudpReliableEventKind::kBattleStart:
            outPacketType = static_cast<uint16_t>(Net::RudpPacketType::kBattleStart);
            outChannelId = static_cast<uint8_t>(Net::RudpChannelId::kEvent);
            return true;
        case Net::RudpReliableEventKind::kMonsterDeath:
        case Net::RudpReliableEventKind::kLootResolved:
            outPacketType = static_cast<uint16_t>(Net::RudpPacketType::kGameEvent);
            outChannelId = static_cast<uint8_t>(Net::RudpChannelId::kEvent);
            return true;
        case Net::RudpReliableEventKind::kMetaResponse:
            outPacketType = static_cast<uint16_t>(Net::RudpPacketType::kMetaResponse);
            outChannelId = static_cast<uint8_t>(Net::RudpChannelId::kControl);
            return true;
    }

    return false;
}
}  // namespace

namespace Net {
RudpReliableEventTrackResult RudpReliableEventSendQueue::track(
    const RudpReliableEventDescriptor& descriptor,
    uint32_t sequence,
    const std::vector<uint8_t>& packetBytes,
    TimePoint now) {
    if (!isValidDescriptor(descriptor)) {
        return RudpReliableEventTrackResult::kInvalidDescriptor;
    }
    if (packetBytes.empty()) {
        return RudpReliableEventTrackResult::kInvalidPacketBytes;
    }
    if (containsSequence(sequence)) {
        return RudpReliableEventTrackResult::kDuplicateSequence;
    }
    if (containsLogicalEvent(descriptor.kind, descriptor.logicalKey)) {
        return RudpReliableEventTrackResult::kDuplicateLogicalEvent;
    }

    if (!reliableQueue_.track(sequence, packetBytes, now)) {
        return RudpReliableEventTrackResult::kDuplicateSequence;
    }
    pendingEntries_.push_back(RudpReliableEventPendingEntry{descriptor, sequence});
    return RudpReliableEventTrackResult::kTracked;
}

size_t RudpReliableEventSendQueue::consumeAck(uint32_t ack, uint32_t ackBits) {
    const size_t consumed = reliableQueue_.consumeAck(ack, ackBits);
    if (consumed == 0) {
        return 0;
    }

    pendingEntries_.erase(
        std::remove_if(
            pendingEntries_.begin(),
            pendingEntries_.end(),
            [this](const RudpReliableEventPendingEntry& entry) {
                return !reliableQueue_.contains(entry.sequence);
            }),
        pendingEntries_.end());
    return consumed;
}

bool RudpReliableEventSendQueue::remove(uint32_t sequence) {
    if (!reliableQueue_.remove(sequence)) {
        return false;
    }

    pendingEntries_.erase(
        std::remove_if(
            pendingEntries_.begin(),
            pendingEntries_.end(),
            [sequence](const RudpReliableEventPendingEntry& entry) {
                return entry.sequence == sequence;
            }),
        pendingEntries_.end());
    return true;
}

std::vector<uint32_t> RudpReliableEventSendQueue::dueForRetransmission(
    TimePoint now) const {
    return reliableQueue_.dueForRetransmission(now);
}

std::vector<uint32_t> RudpReliableEventSendQueue::expiredSequences(
    TimePoint now) const {
    return reliableQueue_.expiredSequences(now);
}

bool RudpReliableEventSendQueue::markRetransmitted(
    uint32_t sequence,
    TimePoint now) {
    return reliableQueue_.markRetransmitted(sequence, now);
}

size_t RudpReliableEventSendQueue::pendingCount() const {
    return pendingEntries_.size();
}

bool RudpReliableEventSendQueue::containsSequence(uint32_t sequence) const {
    return pendingEntry(sequence) != nullptr;
}

bool RudpReliableEventSendQueue::containsLogicalEvent(
    RudpReliableEventKind kind,
    const std::string& logicalKey) const {
    return std::find_if(
        pendingEntries_.begin(),
        pendingEntries_.end(),
        [kind, &logicalKey](const RudpReliableEventPendingEntry& entry) {
            return entry.descriptor.kind == kind &&
                entry.descriptor.logicalKey == logicalKey;
        }) != pendingEntries_.end();
}

const RudpReliableEventPendingEntry* RudpReliableEventSendQueue::pendingEntry(
    uint32_t sequence) const {
    const auto it = findPendingEntry(sequence);
    if (it == pendingEntries_.end()) {
        return nullptr;
    }
    return &(*it);
}

const std::vector<uint8_t>* RudpReliableEventSendQueue::packetBytes(
    uint32_t sequence) const {
    return reliableQueue_.packetBytes(sequence);
}

uint32_t RudpReliableEventSendQueue::retransmissionCount(
    uint32_t sequence) const {
    return reliableQueue_.retransmissionCount(sequence);
}

const RudpReliableEventSendQueue::TimePoint* RudpReliableEventSendQueue::lastSentAt(
    uint32_t sequence) const {
    return reliableQueue_.lastSentAt(sequence);
}

std::vector<uint32_t> RudpReliableEventSendQueue::pendingSequences() const {
    std::vector<uint32_t> sequences;
    sequences.reserve(pendingEntries_.size());
    for (const RudpReliableEventPendingEntry& entry : pendingEntries_) {
        sequences.push_back(entry.sequence);
    }
    return sequences;
}

bool RudpReliableEventSendQueue::isValidDescriptor(
    const RudpReliableEventDescriptor& descriptor) {
    if (descriptor.logicalKey.empty()) {
        return false;
    }

    uint16_t expectedPacketType = 0;
    uint8_t expectedChannelId = 0;
    if (!expectedPacketContract(
            descriptor.kind,
            expectedPacketType,
            expectedChannelId)) {
        return false;
    }

    return descriptor.packetType == expectedPacketType &&
        descriptor.channelId == expectedChannelId;
}

std::vector<RudpReliableEventPendingEntry>::iterator
RudpReliableEventSendQueue::findPendingEntry(uint32_t sequence) {
    return std::find_if(
        pendingEntries_.begin(),
        pendingEntries_.end(),
        [sequence](const RudpReliableEventPendingEntry& entry) {
            return entry.sequence == sequence;
        });
}

std::vector<RudpReliableEventPendingEntry>::const_iterator
RudpReliableEventSendQueue::findPendingEntry(uint32_t sequence) const {
    return std::find_if(
        pendingEntries_.begin(),
        pendingEntries_.end(),
        [sequence](const RudpReliableEventPendingEntry& entry) {
            return entry.sequence == sequence;
        });
}
}  // namespace Net
