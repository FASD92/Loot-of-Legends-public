#pragma once

#include <lol/meta/MetaClaimClient.hpp>

#include <chrono>
#include <cstddef>
#include <string>

namespace lol::meta {

struct CurlHttpsExchangeConfig final {
  std::string caCertificatePath;
  std::string caCertificateSha256;
  std::string expectedHostname;
  std::chrono::milliseconds connectTimeout;
  std::chrono::milliseconds totalTimeout;
  std::size_t maxRequestBytes;
  std::size_t maxResponseBytes;
};

[[nodiscard]] HttpsExchange
makeCurlHttpsExchange(CurlHttpsExchangeConfig config);

} // namespace lol::meta
