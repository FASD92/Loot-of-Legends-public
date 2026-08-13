#include <lol/meta/CurlHttpsExchange.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace lol::meta {
namespace {

constexpr std::size_t kMaximumCaBytes = 1024u * 1024u;

struct CurlCleanup final {
  void operator()(CURL *handle) const noexcept { curl_easy_cleanup(handle); }
};

struct UrlCleanup final {
  void operator()(CURLU *handle) const noexcept { curl_url_cleanup(handle); }
};

struct HeaderCleanup final {
  void operator()(curl_slist *headers) const noexcept {
    curl_slist_free_all(headers);
  }
};

using CurlHandle = std::unique_ptr<CURL, CurlCleanup>;
using UrlHandle = std::unique_ptr<CURLU, UrlCleanup>;
using HeaderList = std::unique_ptr<curl_slist, HeaderCleanup>;

struct ResponseBuffer final {
  std::string body;
  std::size_t maximumBytes{};
  bool overflow{};
};

struct ExchangeState final {
  CurlHttpsExchangeConfig config;
  std::string caCertificate;
};

bool visibleAscii(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x21u || byte > 0x7eu) {
      return false;
    }
  }
  return true;
}

bool validHostname(std::string_view hostname) {
  if (hostname.empty() || hostname.size() > 253u || hostname.front() == '.' ||
      hostname.back() == '.') {
    return false;
  }
  for (const char character : hostname) {
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '.' ||
          character == '-')) {
      return false;
    }
  }
  return true;
}

int hexDigit(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  return -1;
}

std::optional<std::array<unsigned char, 32>>
parseDigest(std::string_view value) {
  if (value.size() != 64u) {
    return std::nullopt;
  }
  std::array<unsigned char, 32> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const int high = hexDigit(value[index * 2u]);
    const int low = hexDigit(value[index * 2u + 1u]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[index] = static_cast<unsigned char>((high << 4) | low);
  }
  return bytes;
}

std::string readPinnedCa(const CurlHttpsExchangeConfig &config) {
  const std::filesystem::path path{config.caCertificatePath};
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || !path.is_absolute() || size == 0u || size > kMaximumCaBytes) {
    throw std::invalid_argument("invalid CA certificate file");
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw std::invalid_argument("invalid CA certificate file");
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!input || input.peek() != std::char_traits<char>::eof()) {
    throw std::invalid_argument("invalid CA certificate file");
  }

  const auto expected = parseDigest(config.caCertificateSha256);
  std::array<unsigned char, EVP_MAX_MD_SIZE> actual{};
  unsigned int actualBytes{};
  if (!expected.has_value() ||
      EVP_Digest(contents.data(), contents.size(), actual.data(), &actualBytes,
                 EVP_sha256(), nullptr) != 1 ||
      actualBytes != expected->size() ||
      CRYPTO_memcmp(actual.data(), expected->data(), expected->size()) != 0) {
    throw std::invalid_argument("CA certificate digest mismatch");
  }
  return contents;
}

bool urlMatches(std::string_view url, std::string_view expectedHostname) {
  UrlHandle handle{curl_url()};
  if (!handle || curl_url_set(handle.get(), CURLUPART_URL,
                              std::string{url}.c_str(), 0u) != CURLUE_OK) {
    return false;
  }
  char *schemeRaw = nullptr;
  char *hostRaw = nullptr;
  const auto schemeStatus =
      curl_url_get(handle.get(), CURLUPART_SCHEME, &schemeRaw, 0u);
  const auto hostStatus =
      curl_url_get(handle.get(), CURLUPART_HOST, &hostRaw, 0u);
  const std::string scheme = schemeRaw == nullptr ? std::string{} : schemeRaw;
  const std::string host = hostRaw == nullptr ? std::string{} : hostRaw;
  curl_free(schemeRaw);
  curl_free(hostRaw);
  return schemeStatus == CURLUE_OK && hostStatus == CURLUE_OK &&
         scheme == "https" && host == expectedHostname;
}

bool validRequest(const ExchangeState &state, const HttpsRequest &request,
                  std::chrono::milliseconds timeout) {
  const bool methodValid = request.method == "GET" || request.method == "POST";
  const bool bodyValid =
      request.body.size() <= state.config.maxRequestBytes &&
      ((request.method == "GET" && request.body.empty() &&
        request.contentType.empty()) ||
       (request.method == "POST" && request.contentType == "application/json"));
  return methodValid && bodyValid &&
         request.authorization.starts_with("Bearer ") &&
         visibleAscii(std::string_view{request.authorization}.substr(
             sizeof("Bearer ") - 1u)) &&
         request.maxResponseBytes > 0u &&
         request.maxResponseBytes <= state.config.maxResponseBytes &&
         timeout > std::chrono::milliseconds::zero() &&
         urlMatches(request.url, state.config.expectedHostname);
}

std::size_t writeBody(char *data, std::size_t size, std::size_t count,
                      void *context) {
  auto &buffer = *static_cast<ResponseBuffer *>(context);
  if (size != 0u && count > std::numeric_limits<std::size_t>::max() / size) {
    buffer.overflow = true;
    return 0u;
  }
  const auto bytes = size * count;
  if (bytes > buffer.maximumBytes - buffer.body.size()) {
    buffer.overflow = true;
    return 0u;
  }
  buffer.body.append(data, bytes);
  return bytes;
}

bool applicationJson(const char *contentType) {
  if (contentType == nullptr) {
    return false;
  }
  std::string_view value{contentType};
  const auto separator = value.find(';');
  value = value.substr(0u, separator);
  while (!value.empty() && value.back() == ' ') {
    value.remove_suffix(1u);
  }
  return value == "application/json";
}

bool setOption(CURL *curl, CURLoption option, long value) {
  return curl_easy_setopt(curl, option, value) == CURLE_OK;
}

HttpsResult exchange(const ExchangeState &state, HttpsRequest request,
                     std::chrono::milliseconds requestedTimeout) {
  if (!validRequest(state, request, requestedTimeout)) {
    return {.status = HttpsStatus::PolicyFailure, .statusCode = 0, .body = {}};
  }

  CurlHandle curl{curl_easy_init()};
  if (!curl) {
    return {.status = HttpsStatus::NetworkFailure, .statusCode = 0, .body = {}};
  }
  HeaderList headers;
  auto appendHeader = [&headers](const std::string &header) {
    auto *next = curl_slist_append(headers.get(), header.c_str());
    if (next == nullptr) {
      return false;
    }
    static_cast<void>(headers.release());
    headers.reset(next);
    return true;
  };
  if (!appendHeader("Accept: application/json") ||
      !appendHeader("Authorization: " + request.authorization) ||
      (!request.contentType.empty() &&
       !appendHeader("Content-Type: " + request.contentType))) {
    return {.status = HttpsStatus::NetworkFailure, .statusCode = 0, .body = {}};
  }

  ResponseBuffer response{
      .body = {}, .maximumBytes = request.maxResponseBytes, .overflow = false};
  curl_blob caBlob{
      .data = const_cast<char *>(state.caCertificate.data()),
      .len = state.caCertificate.size(),
      .flags = CURL_BLOB_NOCOPY,
  };
  const auto totalTimeout =
      std::min(state.config.totalTimeout, requestedTimeout);
  const auto connectTimeout =
      std::min(state.config.connectTimeout, totalTimeout);
  const bool commonOptions =
      curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str()) ==
          CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()) ==
          CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeBody) ==
          CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_CAINFO_BLOB, &caBlob) == CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https") ==
          CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https") ==
          CURLE_OK &&
      curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "") == CURLE_OK &&
      setOption(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L) &&
      setOption(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L) &&
      setOption(curl.get(), CURLOPT_FOLLOWLOCATION, 0L) &&
      setOption(curl.get(), CURLOPT_MAXREDIRS, 0L) &&
      setOption(curl.get(), CURLOPT_NOSIGNAL, 1L) &&
      setOption(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
                connectTimeout.count()) &&
      setOption(curl.get(), CURLOPT_TIMEOUT_MS, totalTimeout.count());
  if (!commonOptions) {
    return {.status = HttpsStatus::NetworkFailure, .statusCode = 0, .body = {}};
  }
  if (request.method == "POST" &&
      (!setOption(curl.get(), CURLOPT_POST, 1L) ||
       curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.data()) !=
           CURLE_OK ||
       curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                        static_cast<curl_off_t>(request.body.size())) !=
           CURLE_OK)) {
    return {.status = HttpsStatus::NetworkFailure, .statusCode = 0, .body = {}};
  }

  const auto result = curl_easy_perform(curl.get());
  if (result == CURLE_OPERATION_TIMEDOUT) {
    return {.status = HttpsStatus::Timeout, .statusCode = 0, .body = {}};
  }
  if (result != CURLE_OK) {
    return {.status = response.overflow ? HttpsStatus::PolicyFailure
                                        : HttpsStatus::NetworkFailure,
            .statusCode = 0,
            .body = {}};
  }

  long statusCode{};
  char *contentType = nullptr;
  if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode) !=
          CURLE_OK ||
      curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &contentType) !=
          CURLE_OK ||
      statusCode < 100L || statusCode > 599L ||
      (!response.body.empty() && !applicationJson(contentType))) {
    return {.status = HttpsStatus::PolicyFailure, .statusCode = 0, .body = {}};
  }
  return {.status = HttpsStatus::Response,
          .statusCode = static_cast<int>(statusCode),
          .body = std::move(response.body)};
}

} // namespace

HttpsExchange makeCurlHttpsExchange(CurlHttpsExchangeConfig config) {
  static const bool curlInitialized =
      curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  if (!validHostname(config.expectedHostname) ||
      config.connectTimeout <= std::chrono::milliseconds::zero() ||
      config.totalTimeout <= std::chrono::milliseconds::zero() ||
      config.connectTimeout > config.totalTimeout ||
      config.maxRequestBytes == 0u || config.maxResponseBytes == 0u ||
      !curlInitialized) {
    throw std::invalid_argument("invalid Curl HTTPS exchange configuration");
  }
  auto caCertificate = readPinnedCa(config);
  auto state = std::make_shared<const ExchangeState>(ExchangeState{
      .config = std::move(config), .caCertificate = std::move(caCertificate)});
  return [state = std::move(state)](HttpsRequest request,
                                    std::chrono::milliseconds timeout) {
    return exchange(*state, std::move(request), timeout);
  };
}

} // namespace lol::meta
