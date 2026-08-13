#include <lol/meta/CurlHttpsExchange.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace {

std::string_view statusName(lol::meta::HttpsStatus status) {
  switch (status) {
  case lol::meta::HttpsStatus::Response:
    return "Response";
  case lol::meta::HttpsStatus::Timeout:
    return "Timeout";
  case lol::meta::HttpsStatus::NetworkFailure:
    return "NetworkFailure";
  case lol::meta::HttpsStatus::PolicyFailure:
    return "PolicyFailure";
  }
  return "Unknown";
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 5) {
    return 64;
  }
  try {
    auto exchange =
        lol::meta::makeCurlHttpsExchange(lol::meta::CurlHttpsExchangeConfig{
            .caCertificatePath = argv[2],
            .caCertificateSha256 = argv[3],
            .expectedHostname = argv[4],
            .connectTimeout = std::chrono::milliseconds{500},
            .totalTimeout = std::chrono::milliseconds{2'000},
            .maxRequestBytes = 64u * 1024u,
            .maxResponseBytes = 64u * 1024u,
        });
    const auto result = exchange(
        lol::meta::HttpsRequest{
            .method = "GET",
            .url = argv[1],
            .authorization = "Bearer fixture-secret-not-an-argument",
            .contentType = {},
            .body = {},
            .maxResponseBytes = 64u * 1024u,
        },
        std::chrono::milliseconds{2'000});
    std::cout << "status=" << statusName(result.status)
              << " code=" << result.statusCode
              << " bodyBytes=" << result.body.size() << '\n';
    return 0;
  } catch (...) {
    std::cout << "status=PolicyFailure code=0 bodyBytes=0\n";
    return 0;
  }
}
