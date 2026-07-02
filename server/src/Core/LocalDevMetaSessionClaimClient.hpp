#pragma once

#include <string_view>

#include "Core/MetaSessionClaimClient.hpp"

namespace Core {

class LocalDevMetaSessionClaimClient final : public IMetaSessionClaimClient {
public:
    static constexpr std::string_view kTokenPrefix = "dev-session:";

    void claimGameSessionAsync(
        const MetaSessionClaimRequest& request,
        ClaimCallback callback) override;
    void releaseGameSessionAsync(const MetaSessionReleaseRequest& request) override;
    void renewGameSessionAsync(const MetaSessionRenewRequest& request) override;
};

}  // namespace Core
