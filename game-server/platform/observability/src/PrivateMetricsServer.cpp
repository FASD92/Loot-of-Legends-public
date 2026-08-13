#include <lol/observability/PrivateMetricsServer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lol::observability {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kPath = "/private/metrics/v1";
constexpr std::size_t kMaximumTokenBytes = 512u;

bool privateIpv4(in_addr parsed) {
  const auto address = ntohl(parsed.s_addr);
  return (address & 0xff000000u) == 0x7f000000u ||
         (address & 0xff000000u) == 0x0a000000u ||
         (address & 0xfff00000u) == 0xac100000u ||
         (address & 0xffff0000u) == 0xc0a80000u;
}

std::optional<in_addr> resolvePrivateIpv4(std::string_view text) {
  const std::string endpoint{text};
  in_addr parsed{};
  if (::inet_pton(AF_INET, endpoint.c_str(), &parsed) == 1) {
    return privateIpv4(parsed) ? std::optional{parsed} : std::nullopt;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo *results{};
  if (endpoint.empty() ||
      ::getaddrinfo(endpoint.c_str(), nullptr, &hints, &results) != 0) {
    return std::nullopt;
  }

  std::optional<in_addr> resolved;
  bool valid = true;
  for (auto *current = results; current != nullptr;
       current = current->ai_next) {
    if (current->ai_addr == nullptr ||
        current->ai_addrlen != sizeof(sockaddr_in)) {
      valid = false;
      break;
    }
    const auto address =
        reinterpret_cast<const sockaddr_in *>(current->ai_addr)->sin_addr;
    if (!privateIpv4(address) ||
        (resolved.has_value() && resolved->s_addr != address.s_addr)) {
      valid = false;
      break;
    }
    resolved = address;
  }
  ::freeaddrinfo(results);
  return valid ? resolved : std::nullopt;
}

bool visibleToken(std::string_view value) {
  if (value.size() < 16u || value.size() > kMaximumTokenBytes) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x21u && byte <= 0x7eu;
  });
}

bool digestText(std::string_view value) {
  return value.size() == 64u && std::ranges::all_of(value, [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::array<unsigned char, 32> sha256(std::string_view value) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int bytes{};
  if (EVP_Digest(value.data(), value.size(), digest.data(), &bytes,
                 EVP_sha256(), nullptr) != 1 ||
      bytes != 32u) {
    throw std::runtime_error("SHA-256 unavailable");
  }
  std::array<unsigned char, 32> result{};
  std::ranges::copy_n(digest.begin(), result.size(), result.begin());
  return result;
}

bool authorized(std::string_view authorization,
                const std::array<unsigned char, 32> &expected) {
  constexpr std::string_view prefix = "Bearer ";
  if (!authorization.starts_with(prefix) ||
      authorization.size() > prefix.size() + kMaximumTokenBytes) {
    return false;
  }
  const auto actual = sha256(authorization.substr(prefix.size()));
  return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

std::string lower(std::string_view value) {
  std::string result{value};
  std::ranges::transform(result, result.begin(), [](char character) {
    if (character >= 'A' && character <= 'Z') {
      return static_cast<char>(character - 'A' + 'a');
    }
    return character;
  });
  return result;
}

struct Request final {
  std::string method;
  std::string path;
  std::string authorization;
};

std::optional<Request> parseRequest(std::string_view raw) {
  const auto headerEnd = raw.find("\r\n\r\n");
  const auto firstEnd = raw.find("\r\n");
  if (headerEnd == std::string_view::npos ||
      firstEnd == std::string_view::npos || firstEnd > headerEnd) {
    return std::nullopt;
  }
  const auto first = raw.substr(0u, firstEnd);
  const auto firstSpace = first.find(' ');
  const auto secondSpace = firstSpace == std::string_view::npos
                               ? std::string_view::npos
                               : first.find(' ', firstSpace + 1u);
  if (firstSpace == std::string_view::npos ||
      secondSpace == std::string_view::npos ||
      first.substr(secondSpace + 1u) != "HTTP/1.1") {
    return std::nullopt;
  }
  Request request{.method = std::string{first.substr(0u, firstSpace)},
                  .path = std::string{first.substr(
                      firstSpace + 1u, secondSpace - firstSpace - 1u)},
                  .authorization = {}};
  bool sawAuthorization = false;
  std::size_t offset = firstEnd + 2u;
  while (offset < headerEnd) {
    const auto end = raw.find("\r\n", offset);
    if (end == std::string_view::npos || end > headerEnd) {
      return std::nullopt;
    }
    const auto line = raw.substr(offset, end - offset);
    const auto separator = line.find(':');
    if (separator == std::string_view::npos) {
      return std::nullopt;
    }
    const auto name = lower(line.substr(0u, separator));
    auto value = line.substr(separator + 1u);
    while (!value.empty() && value.front() == ' ') {
      value.remove_prefix(1u);
    }
    if (name == "authorization") {
      if (sawAuthorization) {
        return std::nullopt;
      }
      sawAuthorization = true;
      request.authorization = std::string{value};
    }
    offset = end + 2u;
  }
  return request;
}

std::optional<std::string> readRequest(int socket,
                                       std::chrono::milliseconds timeout,
                                       std::size_t maximumBytes) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string request;
  std::array<char, 4096> buffer{};
  while (request.find("\r\n\r\n") == std::string::npos) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return std::nullopt;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now +
                                                              1ms);
    pollfd descriptor{.fd = socket, .events = POLLIN, .revents = 0};
    const int ready =
        ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
      return std::nullopt;
    }
    const auto received = ::recv(socket, buffer.data(), buffer.size(), 0);
    if (received <= 0 ||
        static_cast<std::size_t>(received) > maximumBytes - request.size()) {
      return std::nullopt;
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
  return request;
}

std::string number(double value) {
  if (!std::isfinite(value) || value < 0.0) {
    return {};
  }
  std::array<char, 64> buffer{};
  const auto [end, error] = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), value,
      std::chars_format::general, std::numeric_limits<double>::max_digits10);
  return error == std::errc{} ? std::string{buffer.data(), end} : std::string{};
}

void appendMetric(std::string &json, bool &first, std::string_view name,
                  const std::optional<double> &value) {
  if (!value.has_value()) {
    return;
  }
  const auto text = number(*value);
  if (text.empty()) {
    return;
  }
  if (!first) {
    json.push_back(',');
  }
  first = false;
  json.append("{\"name\":\"")
      .append(name)
      .append("\",\"value\":")
      .append(text)
      .push_back('}');
}

std::string snapshotJson(const GameMetricSnapshot &snapshot,
                         std::string_view identity) {
  const auto capturedAt =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  std::string json =
      "{\"schemaVersion\":1,\"source\":\"game\",\"sourceIdentityDigest\":\"";
  json.append(identity)
      .append("\",\"capturedAtUnixMillis\":")
      .append(std::to_string(capturedAt))
      .append(",\"metrics\":[");
  bool first = true;
  appendMetric(json, first, "room_queue_delay_ms", snapshot.roomQueueDelayMs);
  appendMetric(json, first, "room_processing_duration_ms",
               snapshot.roomProcessingDurationMs);
  appendMetric(json, first, "critical_terminal_latency_ms",
               snapshot.criticalTerminalLatencyMs);
  appendMetric(json, first, "active_snapshot_interval_ms",
               snapshot.activeSnapshotIntervalMs);
  appendMetric(json, first, "durable_append_latency_ms",
               snapshot.durableAppendLatencyMs);
  appendMetric(json, first, "outbox_backlog_records",
               snapshot.outboxBacklogRecords);
  appendMetric(json, first, "outbox_oldest_pending_ms",
               snapshot.outboxOldestPendingMs);
  appendMetric(json, first, "outbox_drain_ms", snapshot.outboxDrainMs);
  appendMetric(json, first, "gameplay_progress_total",
               snapshot.gameplayProgressTotal);
  appendMetric(json, first, "process_cpu_busy_ratio",
               snapshot.processCpuBusyRatio);
  appendMetric(json, first, "process_rss_bytes", snapshot.processRssBytes);
  appendMetric(json, first, "process_fd_count", snapshot.processFdCount);
  appendMetric(json, first, "server_invariant_total",
               snapshot.serverInvariantTotal);
  json.append("]}");
  return json;
}

void sendAll(int socket, std::string_view response) {
  std::size_t offset{};
  while (offset < response.size()) {
#if defined(__linux__)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto sent = ::send(socket, response.data() + offset,
                             response.size() - offset, flags);
    if (sent <= 0) {
      return;
    }
    offset += static_cast<std::size_t>(sent);
  }
}

void respond(int socket, int status, std::string_view reason,
             std::string_view body) {
  std::string response =
      "HTTP/1.1 " + std::to_string(status) + " " + std::string{reason} + "\r\n";
  response.append("Content-Type: application/json\r\n")
      .append("Cache-Control: no-store\r\n")
      .append("Connection: close\r\n")
      .append("Content-Length: ")
      .append(std::to_string(body.size()))
      .append("\r\n\r\n")
      .append(body);
  sendAll(socket, response);
}

} // namespace

class PrivateMetricsServer::Impl final {
public:
  Impl(PrivateMetricsServerConfig config, SnapshotSource source)
      : config_(std::move(config)), source_(std::move(source)),
        expectedToken_(sha256(config_.readToken)) {
    const auto parsed = resolvePrivateIpv4(config_.bindAddress);
    if (!parsed.has_value() || !visibleToken(config_.readToken) ||
        !digestText(config_.sourceIdentityDigest) ||
        config_.requestTimeout != 1s || config_.maxRequestBytes == 0u ||
        config_.maxRequestBytes > 64u * 1024u ||
        config_.maxResponseBytes == 0u ||
        config_.maxResponseBytes > 64u * 1024u || !source_) {
      throw std::invalid_argument(
          "invalid private metrics server configuration");
    }
    address_ = *parsed;
  }

  ~Impl() { stop(); }

  bool start() {
    if (thread_.joinable()) {
      return false;
    }
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) {
      return false;
    }
    int enabled = 1;
    static_cast<void>(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                                   &enabled, sizeof(enabled)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr = address_;
    address.sin_port = htons(config_.port);
    if (::bind(listener_, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 8) != 0) {
      ::close(listener_);
      listener_ = -1;
      return false;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0) {
      ::close(listener_);
      listener_ = -1;
      return false;
    }
    port_.store(ntohs(address.sin_port), std::memory_order_release);
    stopping_.store(false, std::memory_order_release);
    try {
      thread_ = std::thread{[this] { run(); }};
    } catch (...) {
      ::close(listener_);
      listener_ = -1;
      port_.store(0u, std::memory_order_release);
      return false;
    }
    return true;
  }

  void stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (listener_ >= 0) {
      static_cast<void>(::shutdown(listener_, SHUT_RDWR));
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (listener_ >= 0) {
      ::close(listener_);
      listener_ = -1;
    }
    port_.store(0u, std::memory_order_release);
  }

  std::uint16_t port() const noexcept {
    return port_.load(std::memory_order_acquire);
  }

private:
  void run() noexcept {
    while (!stopping_.load(std::memory_order_acquire)) {
      pollfd descriptor{.fd = listener_, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&descriptor, 1, 100);
      if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
        continue;
      }
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
#if defined(__APPLE__)
      int enabled = 1;
      static_cast<void>(::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                                     sizeof(enabled)));
#endif
      handle(client);
      ::close(client);
    }
  }

  void handle(int client) noexcept {
    try {
      const auto raw =
          readRequest(client, config_.requestTimeout, config_.maxRequestBytes);
      if (!raw.has_value()) {
        respond(client, 408, "Request Timeout",
                "{\"code\":\"REQUEST_TIMEOUT\"}");
        return;
      }
      const auto request = parseRequest(*raw);
      if (!request.has_value()) {
        respond(client, 400, "Bad Request", "{\"code\":\"INVALID\"}");
        return;
      }
      if (!authorized(request->authorization, expectedToken_)) {
        respond(client, 401, "Unauthorized", "{\"code\":\"UNAUTHORIZED\"}");
        return;
      }
      if (request->path != kPath) {
        respond(client, 404, "Not Found", "{\"code\":\"NOT_FOUND\"}");
        return;
      }
      if (request->method != "GET") {
        respond(client, 405, "Method Not Allowed",
                "{\"code\":\"METHOD_NOT_ALLOWED\"}");
        return;
      }
      const auto body = snapshotJson(source_(), config_.sourceIdentityDigest);
      if (body.size() > config_.maxResponseBytes) {
        respond(client, 503, "Service Unavailable",
                "{\"code\":\"RESPONSE_BOUNDED\"}");
        return;
      }
      respond(client, 200, "OK", body);
    } catch (...) {
      respond(client, 503, "Service Unavailable", "{\"code\":\"UNAVAILABLE\"}");
    }
  }

  PrivateMetricsServerConfig config_;
  SnapshotSource source_;
  std::array<unsigned char, 32> expectedToken_;
  in_addr address_{};
  int listener_{-1};
  std::atomic<std::uint16_t> port_{0};
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

PrivateMetricsServer::PrivateMetricsServer(PrivateMetricsServerConfig config,
                                           SnapshotSource source)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(source))) {}

PrivateMetricsServer::~PrivateMetricsServer() = default;

bool PrivateMetricsServer::start() { return impl_->start(); }

void PrivateMetricsServer::stop() noexcept { impl_->stop(); }

std::uint16_t PrivateMetricsServer::port() const noexcept {
  return impl_->port();
}

} // namespace lol::observability
