#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Net/RudpMetaResponsePayload.hpp"

namespace {
constexpr char kSettlementId[] = "room-42-session-1001-finish-1";

std::vector<uint8_t> validRetryLaterBytes() {
    return std::vector<uint8_t>{
        0x00, 0x04,
        0x00, 0x25,
        0x00, 0x1D,
        'r', 'o', 'o', 'm', '-', '4', '2', '-', 's', 'e', 's', 's', 'i', 'o', 'n',
        '-', '1', '0', '0', '1', '-', 'f', 'i', 'n', 'i', 's', 'h', '-', '1',
        0x00, 0x04,
        0x00, 0x00, 0x05, 0xDC};
}

Net::RudpMetaResponsePayload payload(
    Net::RudpMetaResponseOp op,
    Net::RudpMetaResponseStatus status,
    uint32_t retryAfterMs = 0) {
    return Net::RudpMetaResponsePayload{op, kSettlementId, status, retryAfterMs};
}
}  // namespace

TEST(RudpMetaResponsePayloadTests, SerializesRetryLaterInBigEndianOrder) {
    const Net::RudpMetaResponsePayload response = payload(
        Net::RudpMetaResponseOp::kRetryLater,
        Net::RudpMetaResponseStatus::kRetryLater,
        1500);

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpMetaResponsePayload(response, bytes));

    EXPECT_EQ(bytes, validRetryLaterBytes());
}

TEST(RudpMetaResponsePayloadTests, ParsesRetryLaterPayload) {
    const std::vector<uint8_t> bytes = validRetryLaterBytes();

    Net::RudpMetaResponsePayload parsed;
    ASSERT_TRUE(Net::parseRudpMetaResponsePayload(bytes.data(), bytes.size(), parsed));

    EXPECT_EQ(parsed.op, Net::RudpMetaResponseOp::kRetryLater);
    EXPECT_EQ(parsed.settlementId, kSettlementId);
    EXPECT_EQ(parsed.status, Net::RudpMetaResponseStatus::kRetryLater);
    EXPECT_EQ(parsed.retryAfterMs, 1500U);
}

TEST(RudpMetaResponsePayloadTests, RoundTripsTerminalAndRetryStatuses) {
    const std::vector<Net::RudpMetaResponsePayload> responses = {
        payload(Net::RudpMetaResponseOp::kApplied, Net::RudpMetaResponseStatus::kApplied),
        payload(Net::RudpMetaResponseOp::kDuplicate, Net::RudpMetaResponseStatus::kDuplicate),
        payload(Net::RudpMetaResponseOp::kRejected, Net::RudpMetaResponseStatus::kRejected),
        payload(
            Net::RudpMetaResponseOp::kRetryLater,
            Net::RudpMetaResponseStatus::kRetryLater,
            250),
    };

    for (const Net::RudpMetaResponsePayload& response : responses) {
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(Net::serializeRudpMetaResponsePayload(response, bytes));

        Net::RudpMetaResponsePayload parsed;
        ASSERT_TRUE(Net::parseRudpMetaResponsePayload(bytes.data(), bytes.size(), parsed));
        EXPECT_EQ(parsed.op, response.op);
        EXPECT_EQ(parsed.settlementId, response.settlementId);
        EXPECT_EQ(parsed.status, response.status);
        EXPECT_EQ(parsed.retryAfterMs, response.retryAfterMs);
    }
}

TEST(RudpMetaResponsePayloadTests, ComputesBodyLengthFromSettlementIdLength) {
    Net::RudpMetaResponsePayload response = payload(
        Net::RudpMetaResponseOp::kApplied,
        Net::RudpMetaResponseStatus::kApplied);
    response.settlementId = "abc";

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpMetaResponsePayload(response, bytes));

    EXPECT_EQ(bytes.size(), Net::rudpMetaResponsePayloadSize(3));
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x0B);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x03);
}

TEST(RudpMetaResponsePayloadTests, RejectsInvalidSettlementIdsOnSerialize) {
    const std::string tooLong(Net::kRudpMetaResponseSettlementIdMaxLength + 1, 'a');
    const std::vector<std::string> invalidIds = {
        "",
        tooLong,
        "room-42\nsession-1001",
        std::string("room-42-session-") + static_cast<char>(0x7F),
    };

    for (const std::string& settlementId : invalidIds) {
        Net::RudpMetaResponsePayload response = payload(
            Net::RudpMetaResponseOp::kApplied,
            Net::RudpMetaResponseStatus::kApplied);
        response.settlementId = settlementId;

        std::vector<uint8_t> bytes{0xAA, 0xBB};
        EXPECT_FALSE(Net::serializeRudpMetaResponsePayload(response, bytes));
        EXPECT_TRUE(bytes.empty());
    }
}

TEST(RudpMetaResponsePayloadTests, RejectsInvalidSettlementIdsOnParse) {
    std::vector<uint8_t> emptyId = validRetryLaterBytes();
    emptyId[5] = 0x00;
    emptyId.erase(emptyId.begin() + 6, emptyId.begin() + 6 + 29);
    emptyId[3] = 0x08;

    std::vector<uint8_t> newlineId = validRetryLaterBytes();
    newlineId[10] = '\n';

    std::vector<uint8_t> deleteId = validRetryLaterBytes();
    deleteId[10] = 0x7F;

    const std::vector<std::vector<uint8_t>> invalidPayloads = {
        emptyId,
        newlineId,
        deleteId,
    };

    for (const std::vector<uint8_t>& bytes : invalidPayloads) {
        Net::RudpMetaResponsePayload parsed{
            Net::RudpMetaResponseOp::kApplied,
            "previous",
            Net::RudpMetaResponseStatus::kApplied,
            1};
        EXPECT_FALSE(Net::parseRudpMetaResponsePayload(bytes.data(), bytes.size(), parsed));
        EXPECT_EQ(parsed.op, Net::RudpMetaResponseOp::kApplied);
        EXPECT_TRUE(parsed.settlementId.empty());
        EXPECT_EQ(parsed.status, Net::RudpMetaResponseStatus::kApplied);
        EXPECT_EQ(parsed.retryAfterMs, 0U);
    }
}

TEST(RudpMetaResponsePayloadTests, RejectsInvalidPayloadSizes) {
    std::vector<uint8_t> tooShort(Net::kRudpMetaResponseFixedPayloadSize - 1, 0);
    std::vector<uint8_t> truncated = validRetryLaterBytes();
    truncated.pop_back();
    std::vector<uint8_t> tooLong = validRetryLaterBytes();
    tooLong.push_back(0x00);

    const std::vector<std::vector<uint8_t>> invalidPayloads = {
        tooShort,
        truncated,
        tooLong,
    };

    for (const std::vector<uint8_t>& bytes : invalidPayloads) {
        Net::RudpMetaResponsePayload parsed{
            Net::RudpMetaResponseOp::kRejected,
            "previous",
            Net::RudpMetaResponseStatus::kRejected,
            1};
        EXPECT_FALSE(Net::parseRudpMetaResponsePayload(bytes.data(), bytes.size(), parsed));
        EXPECT_EQ(parsed.op, Net::RudpMetaResponseOp::kApplied);
        EXPECT_TRUE(parsed.settlementId.empty());
        EXPECT_EQ(parsed.status, Net::RudpMetaResponseStatus::kApplied);
        EXPECT_EQ(parsed.retryAfterMs, 0U);
    }
}

TEST(RudpMetaResponsePayloadTests, RejectsBodyLengthMismatch) {
    std::vector<uint8_t> bytes = validRetryLaterBytes();
    bytes[3] = 0x24;
    Net::RudpMetaResponsePayload parsed;

    EXPECT_FALSE(Net::parseRudpMetaResponsePayload(bytes.data(), bytes.size(), parsed));
}

TEST(RudpMetaResponsePayloadTests, RejectsSettlementLengthMismatch) {
    std::vector<uint8_t> bytes = validRetryLaterBytes();
    bytes[5] = 0x1C;
    Net::RudpMetaResponsePayload parsed;

    EXPECT_FALSE(Net::parseRudpMetaResponsePayload(bytes.data(), bytes.size(), parsed));
}

TEST(RudpMetaResponsePayloadTests, RejectsInvalidOpAndStatusWireValues) {
    std::vector<uint8_t> invalidOp = validRetryLaterBytes();
    invalidOp[1] = 0x7F;
    std::vector<uint8_t> invalidStatus = validRetryLaterBytes();
    invalidStatus[36] = 0x7F;
    Net::RudpMetaResponsePayload parsed;

    EXPECT_FALSE(Net::parseRudpMetaResponsePayload(
        invalidOp.data(),
        invalidOp.size(),
        parsed));
    EXPECT_FALSE(Net::parseRudpMetaResponsePayload(
        invalidStatus.data(),
        invalidStatus.size(),
        parsed));
}

TEST(RudpMetaResponsePayloadTests, RejectsOpStatusMismatchOnParse) {
    std::vector<uint8_t> bytes = validRetryLaterBytes();
    bytes[1] = 0x01;
    Net::RudpMetaResponsePayload parsed;

    EXPECT_FALSE(Net::parseRudpMetaResponsePayload(bytes.data(), bytes.size(), parsed));
}

TEST(RudpMetaResponsePayloadTests, RejectsOpStatusMismatchOnSerialize) {
    const Net::RudpMetaResponsePayload response{
        Net::RudpMetaResponseOp::kApplied,
        kSettlementId,
        Net::RudpMetaResponseStatus::kRetryLater,
        1500};

    std::vector<uint8_t> bytes{0xAA, 0xBB};
    EXPECT_FALSE(Net::serializeRudpMetaResponsePayload(response, bytes));
    EXPECT_TRUE(bytes.empty());
}
