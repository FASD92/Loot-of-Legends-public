#include "Net/RudpMetaResponsePayload.hpp"

#include <algorithm>
#include <limits>

namespace {
void writeU16BE(uint16_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}

void writeU32BE(uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t readU16BE(const uint8_t* data) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) |
        static_cast<uint16_t>(data[1]));
}

uint32_t readU32BE(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}

bool isValidSettlementId(const std::string& settlementId) {
    if (settlementId.empty() ||
        settlementId.size() > Net::kRudpMetaResponseSettlementIdMaxLength) {
        return false;
    }

    for (const unsigned char ch : settlementId) {
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
    }

    return true;
}

bool opToWire(Net::RudpMetaResponseOp op, uint16_t& outWire) {
    switch (op) {
        case Net::RudpMetaResponseOp::kApplied:
            outWire = 1;
            return true;
        case Net::RudpMetaResponseOp::kDuplicate:
            outWire = 2;
            return true;
        case Net::RudpMetaResponseOp::kRejected:
            outWire = 3;
            return true;
        case Net::RudpMetaResponseOp::kRetryLater:
            outWire = 4;
            return true;
    }

    return false;
}

bool wireToOp(uint16_t wire, Net::RudpMetaResponseOp& outOp) {
    switch (wire) {
        case 1:
            outOp = Net::RudpMetaResponseOp::kApplied;
            return true;
        case 2:
            outOp = Net::RudpMetaResponseOp::kDuplicate;
            return true;
        case 3:
            outOp = Net::RudpMetaResponseOp::kRejected;
            return true;
        case 4:
            outOp = Net::RudpMetaResponseOp::kRetryLater;
            return true;
        default:
            return false;
    }
}

bool statusToWire(Net::RudpMetaResponseStatus status, uint16_t& outWire) {
    switch (status) {
        case Net::RudpMetaResponseStatus::kApplied:
            outWire = 1;
            return true;
        case Net::RudpMetaResponseStatus::kDuplicate:
            outWire = 2;
            return true;
        case Net::RudpMetaResponseStatus::kRejected:
            outWire = 3;
            return true;
        case Net::RudpMetaResponseStatus::kRetryLater:
            outWire = 4;
            return true;
    }

    return false;
}

bool wireToStatus(uint16_t wire, Net::RudpMetaResponseStatus& outStatus) {
    switch (wire) {
        case 1:
            outStatus = Net::RudpMetaResponseStatus::kApplied;
            return true;
        case 2:
            outStatus = Net::RudpMetaResponseStatus::kDuplicate;
            return true;
        case 3:
            outStatus = Net::RudpMetaResponseStatus::kRejected;
            return true;
        case 4:
            outStatus = Net::RudpMetaResponseStatus::kRetryLater;
            return true;
        default:
            return false;
    }
}

bool opMatchesStatus(
    Net::RudpMetaResponseOp op,
    Net::RudpMetaResponseStatus status) {
    uint16_t opWire = 0;
    uint16_t statusWire = 0;
    return opToWire(op, opWire) &&
        statusToWire(status, statusWire) &&
        opWire == statusWire;
}

bool isValidPayload(const Net::RudpMetaResponsePayload& payload) {
    return isValidSettlementId(payload.settlementId) &&
        opMatchesStatus(payload.op, payload.status);
}
}  // namespace

namespace Net {
size_t rudpMetaResponsePayloadSize(size_t settlementIdLength) {
    return kRudpMetaResponseFixedPayloadSize + settlementIdLength;
}

bool serializeRudpMetaResponsePayload(
    const RudpMetaResponsePayload& payload,
    std::vector<uint8_t>& outPayload) {
    outPayload.clear();
    if (!isValidPayload(payload) ||
        payload.settlementId.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    uint16_t opWire = 0;
    uint16_t statusWire = 0;
    if (!opToWire(payload.op, opWire) || !statusToWire(payload.status, statusWire)) {
        return false;
    }

    const size_t bodyLen = kRudpMetaResponseBodyFixedSize + payload.settlementId.size();
    if (bodyLen > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    outPayload.assign(rudpMetaResponsePayloadSize(payload.settlementId.size()), 0);
    size_t offset = 0;
    writeU16BE(opWire, outPayload.data() + offset);
    offset += kRudpMetaResponseOpFieldSize;
    writeU16BE(static_cast<uint16_t>(bodyLen), outPayload.data() + offset);
    offset += kRudpMetaResponseBodyLenFieldSize;
    writeU16BE(
        static_cast<uint16_t>(payload.settlementId.size()),
        outPayload.data() + offset);
    offset += kRudpMetaResponseSettlementIdLenFieldSize;
    std::copy(
        payload.settlementId.begin(),
        payload.settlementId.end(),
        outPayload.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset));
    offset += payload.settlementId.size();
    writeU16BE(statusWire, outPayload.data() + offset);
    offset += kRudpMetaResponseStatusFieldSize;
    writeU32BE(payload.retryAfterMs, outPayload.data() + offset);
    return true;
}

bool parseRudpMetaResponsePayload(
    const uint8_t* data,
    size_t size,
    RudpMetaResponsePayload& outPayload) {
    outPayload = RudpMetaResponsePayload{};
    if (data == nullptr || size < kRudpMetaResponseFixedPayloadSize) {
        return false;
    }

    size_t offset = 0;
    RudpMetaResponseOp op{};
    if (!wireToOp(readU16BE(data + offset), op)) {
        return false;
    }
    offset += kRudpMetaResponseOpFieldSize;

    const uint16_t bodyLen = readU16BE(data + offset);
    offset += kRudpMetaResponseBodyLenFieldSize;
    if (static_cast<size_t>(bodyLen) + kRudpMetaResponseOpFieldSize +
            kRudpMetaResponseBodyLenFieldSize != size ||
        bodyLen < kRudpMetaResponseBodyFixedSize) {
        return false;
    }

    const uint16_t settlementIdLength = readU16BE(data + offset);
    offset += kRudpMetaResponseSettlementIdLenFieldSize;
    const size_t expectedBodyLen =
        kRudpMetaResponseBodyFixedSize + static_cast<size_t>(settlementIdLength);
    if (expectedBodyLen != bodyLen ||
        offset + settlementIdLength + kRudpMetaResponseStatusFieldSize +
            kRudpMetaResponseRetryAfterMsFieldSize != size) {
        return false;
    }

    std::string settlementId(
        reinterpret_cast<const char*>(data + offset),
        reinterpret_cast<const char*>(data + offset + settlementIdLength));
    if (!isValidSettlementId(settlementId)) {
        return false;
    }
    offset += settlementIdLength;

    RudpMetaResponseStatus status{};
    if (!wireToStatus(readU16BE(data + offset), status)) {
        return false;
    }
    offset += kRudpMetaResponseStatusFieldSize;

    if (!opMatchesStatus(op, status)) {
        return false;
    }

    outPayload.op = op;
    outPayload.settlementId = std::move(settlementId);
    outPayload.status = status;
    outPayload.retryAfterMs = readU32BE(data + offset);
    return true;
}
}  // namespace Net
