#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Net/RudpMetaResponseIdempotencyTracker.hpp"

namespace Net {
constexpr size_t kRudpMetaResponseOpFieldSize = 2;
constexpr size_t kRudpMetaResponseBodyLenFieldSize = 2;
constexpr size_t kRudpMetaResponseSettlementIdLenFieldSize = 2;
constexpr size_t kRudpMetaResponseStatusFieldSize = 2;
constexpr size_t kRudpMetaResponseRetryAfterMsFieldSize = 4;
constexpr size_t kRudpMetaResponseBodyFixedSize =
    kRudpMetaResponseSettlementIdLenFieldSize + kRudpMetaResponseStatusFieldSize +
    kRudpMetaResponseRetryAfterMsFieldSize;
constexpr size_t kRudpMetaResponseFixedPayloadSize =
    kRudpMetaResponseOpFieldSize + kRudpMetaResponseBodyLenFieldSize +
    kRudpMetaResponseBodyFixedSize;

enum class RudpMetaResponseOp : uint16_t {
    kApplied = 1,
    kDuplicate = 2,
    kRejected = 3,
    kRetryLater = 4,
};

struct RudpMetaResponsePayload {
    RudpMetaResponseOp op{RudpMetaResponseOp::kApplied};
    std::string settlementId;
    RudpMetaResponseStatus status{RudpMetaResponseStatus::kApplied};
    uint32_t retryAfterMs{0};
};

size_t rudpMetaResponsePayloadSize(size_t settlementIdLength);

bool serializeRudpMetaResponsePayload(
    const RudpMetaResponsePayload& payload,
    std::vector<uint8_t>& outPayload);

bool parseRudpMetaResponsePayload(
    const uint8_t* data,
    size_t size,
    RudpMetaResponsePayload& outPayload);
}  // namespace Net
