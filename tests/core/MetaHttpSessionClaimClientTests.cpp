#include <cstdint>
#include <chrono>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Core/MetaHttpSessionClaimClient.hpp"

namespace {

std::string chunkedHttpResponse(
    const std::vector<std::string>& chunks,
    std::string_view transferEncodingHeader = "Transfer-Encoding: chunked") {
    std::string response =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
    response += transferEncodingHeader;
    response += "\r\n\r\n";
    for (const std::string& chunk : chunks) {
        std::ostringstream size;
        size << std::hex << chunk.size();
        response += size.str();
        response += "\r\n";
        response += chunk;
        response += "\r\n";
    }
    response += "0\r\n\r\n";
    return response;
}

TEST(MetaHttpSessionClaimClientTests, ParsesHttpBaseUrlWithDefaultPortAndPath) {
    Core::MetaHttpEndpoint endpoint;

    ASSERT_TRUE(Core::parseMetaHttpEndpoint(
        "http://127.0.0.1:8081",
        endpoint));

    EXPECT_EQ(endpoint.host, "127.0.0.1");
    EXPECT_EQ(endpoint.port, 8081);
    EXPECT_EQ(endpoint.basePath, "");
}

TEST(MetaHttpSessionClaimClientTests, RejectsUnsupportedHttpsBaseUrl) {
    Core::MetaHttpEndpoint endpoint;

    EXPECT_FALSE(Core::parseMetaHttpEndpoint(
        "https://127.0.0.1:8081",
        endpoint));
}

TEST(MetaHttpSessionClaimClientTests, RejectsHttpBaseUrlWithQueryFragmentOrSpace) {
    Core::MetaHttpEndpoint endpoint;

    EXPECT_FALSE(Core::parseMetaHttpEndpoint("http://localhost?x", endpoint));
    EXPECT_FALSE(Core::parseMetaHttpEndpoint("http://localhost#x", endpoint));
    EXPECT_FALSE(Core::parseMetaHttpEndpoint("http://localhost/foo bar", endpoint));
}

TEST(MetaHttpSessionClaimClientTests, BuildsClaimRequestBody) {
    const Core::MetaSessionClaimRequest request{
        42,
        "token-123"};

    EXPECT_EQ(
        Core::buildMetaClaimRequestBody(request),
        "{\"gameSessionToken\":\"token-123\",\"connectionId\":\"42\"}");
}

TEST(MetaHttpSessionClaimClientTests, InvokesRejectedClaimCallbackSynchronously) {
    Core::MetaHttpSessionClaimClient client(
        Core::MetaHttpEndpoint{},
        "internal-token");
    std::promise<std::thread::id> callbackThreadPromise;
    std::future<std::thread::id> callbackThread = callbackThreadPromise.get_future();
    const std::thread::id callerThread = std::this_thread::get_id();

    client.claimGameSessionAsync(
        Core::MetaSessionClaimRequest{42, "token-123"},
        [&callbackThreadPromise](Core::MetaSessionClaimResult result) {
            EXPECT_FALSE(result.accepted);
            callbackThreadPromise.set_value(std::this_thread::get_id());
        });

    ASSERT_EQ(
        callbackThread.wait_for(std::chrono::milliseconds(0)),
        std::future_status::ready);
    EXPECT_EQ(callbackThread.get(), callerThread);
}

TEST(MetaHttpSessionClaimClientTests, ParsesAcceptedClaimResponse) {
    Core::MetaSessionClaimResult result;

    ASSERT_TRUE(Core::parseMetaClaimResponseBody(
        "{\"status\":\"Accepted\",\"accountId\":123,\"nickname\":\"player123\","
        "\"replacedSessionId\":null,\"reservationExpiresAt\":9999}",
        result));

    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.profile.accountId, 123u);
    EXPECT_EQ(result.profile.nickname, "player123");
    EXPECT_EQ(result.reservationExpiresAtUnixMs, 9999u);
}

TEST(MetaHttpSessionClaimClientTests, DecodesChunkedAcceptedClaimResponseBeforeParsing) {
    const std::string acceptedBody =
        "{\"status\":\"Accepted\",\"accountId\":123,\"nickname\":\"player123\","
        "\"replacedSessionId\":null,\"reservationExpiresAt\":9999}";
    int statusCode = 0;
    std::string decodedBody;

    ASSERT_TRUE(Core::parseMetaHttpResponse(
        chunkedHttpResponse({
            acceptedBody.substr(0, 17),
            acceptedBody.substr(17, 31),
            acceptedBody.substr(48),
        }),
        statusCode,
        decodedBody));
    EXPECT_EQ(statusCode, 200);
    EXPECT_EQ(decodedBody, acceptedBody);

    Core::MetaSessionClaimResult result;
    ASSERT_TRUE(Core::parseMetaClaimResponseBody(decodedBody, result));
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.profile.accountId, 123u);
    EXPECT_EQ(result.profile.nickname, "player123");
    EXPECT_EQ(result.reservationExpiresAtUnixMs, 9999u);
}

TEST(MetaHttpSessionClaimClientTests, DecodesChunkedRejectedClaimResponseBeforeParsing) {
    const std::string rejectedBody = "{\"status\":\"Rejected\"}";
    int statusCode = 0;
    std::string decodedBody;

    ASSERT_TRUE(Core::parseMetaHttpResponse(
        chunkedHttpResponse(
            {rejectedBody.substr(0, 8), rejectedBody.substr(8)},
            "transfer-encoding: Chunked"),
        statusCode,
        decodedBody));
    EXPECT_EQ(statusCode, 200);
    EXPECT_EQ(decodedBody, rejectedBody);

    Core::MetaSessionClaimResult result;
    ASSERT_TRUE(Core::parseMetaClaimResponseBody(decodedBody, result));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.profile.accountId, 0u);
}

TEST(MetaHttpSessionClaimClientTests, RejectsMalformedChunkedClaimResponse) {
    int statusCode = 0;
    std::string decodedBody;

    EXPECT_FALSE(Core::parseMetaHttpResponse(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\n{\"sta\r\n"
        "not-hex\r\nbad\r\n"
        "0\r\n\r\n",
        statusCode,
        decodedBody));
}

TEST(MetaHttpSessionClaimClientTests, RejectsAcceptedClaimResponseWithMalformedNumber) {
    Core::MetaSessionClaimResult result;

    EXPECT_FALSE(Core::parseMetaClaimResponseBody(
        "{\"status\":\"Accepted\",\"accountId\":123abc,\"nickname\":\"player123\","
        "\"reservationExpiresAt\":9999}",
        result));
}

TEST(MetaHttpSessionClaimClientTests, RejectsAcceptedClaimResponseWithTrailingGarbage) {
    Core::MetaSessionClaimResult result;

    EXPECT_FALSE(Core::parseMetaClaimResponseBody(
        "{\"status\":\"Accepted\",\"accountId\":123,\"nickname\":\"player123\","
        "\"reservationExpiresAt\":9999} garbage",
        result));
}

TEST(MetaHttpSessionClaimClientTests, RejectsAcceptedClaimResponseWithEmptyIdentityFields) {
    Core::MetaSessionClaimResult zeroAccount;
    Core::MetaSessionClaimResult emptyNickname;
    Core::MetaSessionClaimResult zeroExpiry;

    EXPECT_FALSE(Core::parseMetaClaimResponseBody(
        "{\"status\":\"Accepted\",\"accountId\":0,\"nickname\":\"player123\","
        "\"reservationExpiresAt\":9999}",
        zeroAccount));
    EXPECT_FALSE(Core::parseMetaClaimResponseBody(
        "{\"status\":\"Accepted\",\"accountId\":123,\"nickname\":\"\","
        "\"reservationExpiresAt\":9999}",
        emptyNickname));
    EXPECT_FALSE(Core::parseMetaClaimResponseBody(
        "{\"status\":\"Accepted\",\"accountId\":123,\"nickname\":\"player123\","
        "\"reservationExpiresAt\":0}",
        zeroExpiry));
}

TEST(MetaHttpSessionClaimClientTests, ParsesRejectedClaimResponseAsRejectedResult) {
    Core::MetaSessionClaimResult result;

    ASSERT_TRUE(Core::parseMetaClaimResponseBody(
        "{\"status\":\"Rejected\"}",
        result));

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.profile.accountId, 0u);
}

TEST(MetaHttpSessionClaimClientTests, BuildsReleaseRequestBody) {
    const Core::MetaSessionReleaseRequest request{
        123,
        1001,
        42};

    EXPECT_EQ(
        Core::buildMetaReleaseRequestBody(request),
        "{\"accountId\":123,\"connectionId\":\"42\"}");
}

TEST(MetaHttpSessionClaimClientTests, BuildsRenewRequestBody) {
    const Core::MetaSessionRenewRequest request{
        123,
        42};

    EXPECT_EQ(
        Core::buildMetaRenewRequestBody(request),
        "{\"accountId\":123,\"connectionId\":\"42\"}");
}

}  // namespace
