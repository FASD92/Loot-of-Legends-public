#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Core/MetaSessionClaimClient.hpp"

namespace Core {

struct MetaHttpEndpoint {
    std::string host;
    uint16_t port{80};
    std::string basePath;
};

bool parseMetaHttpEndpoint(std::string_view baseUrl, MetaHttpEndpoint& outEndpoint);

std::string buildMetaClaimRequestBody(const MetaSessionClaimRequest& request);

std::string buildMetaReleaseRequestBody(const MetaSessionReleaseRequest& request);

std::string buildMetaRenewRequestBody(const MetaSessionRenewRequest& request);

bool parseMetaClaimResponseBody(
    std::string_view body,
    MetaSessionClaimResult& outResult);

bool parseMetaHttpResponse(
    std::string_view response,
    int& outStatusCode,
    std::string& outBody);

class MetaHttpSessionClaimClient final : public IMetaSessionClaimClient {
public:
    MetaHttpSessionClaimClient(
        MetaHttpEndpoint endpoint,
        std::string internalToken);

    void claimGameSessionAsync(
        const MetaSessionClaimRequest& request,
        ClaimCallback callback) override;

    void releaseGameSessionAsync(const MetaSessionReleaseRequest& request) override;

    void renewGameSessionAsync(const MetaSessionRenewRequest& request) override;

private:
    static bool postJson(
        const MetaHttpEndpoint& endpoint,
        std::string_view internalToken,
        std::string_view path,
        std::string_view body,
        int& outStatusCode,
        std::string& outBody);

    MetaHttpEndpoint endpoint_;
    std::string internalToken_;
};

}  // namespace Core
