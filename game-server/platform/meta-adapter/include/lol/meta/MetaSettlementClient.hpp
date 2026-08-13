#pragma once

#include <lol/meta/MetaClaimClient.hpp>
#include <lol/settlement/SettlementPublisher.hpp>

#include <chrono>
#include <cstddef>
#include <string>

namespace lol::meta {

struct MetaSettlementClientConfig final {
  std::string settlementsUrl;
  std::string serviceCredential;
  std::chrono::milliseconds timeout;
  std::size_t maxResponseBytes;
};

class MetaSettlementClient final : public settlement::SettlementMetaPort {
public:
  MetaSettlementClient(MetaSettlementClientConfig config,
                       HttpsExchange exchange);

  [[nodiscard]] settlement::MetaPublishOutcome
  publish(const settlement::DurableSettlementIntent &intent) override;
  [[nodiscard]] settlement::MetaStatusOutcome
  status(const settlement::DurableSettlementIntent &intent) override;

private:
  [[nodiscard]] std::string
  settlementId(const settlement::DurableSettlementIntent &intent) const;

  MetaSettlementClientConfig config_;
  HttpsExchange exchange_;
};

} // namespace lol::meta
