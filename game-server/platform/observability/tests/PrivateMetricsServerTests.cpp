#include <lol/observability/GameMetrics.hpp>
#include <lol/observability/PrivateMetricsServer.hpp>

#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using lol::observability::GameMetrics;
using lol::observability::GameMetricSnapshot;
using lol::observability::PrivateMetricsServer;
using lol::observability::PrivateMetricsServerConfig;

std::string request(std::uint16_t port, std::string_view message) {
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) {
    return {};
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
      ::connect(socket, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
    ::close(socket);
    return {};
  }
  const auto sent = ::send(socket, message.data(), message.size(), 0);
  if (sent != static_cast<ssize_t>(message.size())) {
    ::close(socket);
    return {};
  }
  static_cast<void>(::shutdown(socket, SHUT_WR));
  std::string response;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto received = ::recv(socket, buffer.data(), buffer.size(), 0);
    if (received <= 0) {
      break;
    }
    response.append(buffer.data(), static_cast<std::size_t>(received));
  }
  ::close(socket);
  return response;
}

bool containsNoForbiddenIdentity(std::string_view response) {
  for (const auto forbidden : {"accountId", "sessionId", "roomId",
                               "battleInstanceId", "nickname", "token"}) {
    if (response.find(forbidden) != std::string_view::npos) {
      return false;
    }
  }
  return true;
}

bool gameMetricsPreserveOnlyBoundedAggregates() {
  GameMetrics metrics{2u};
  const auto now = std::chrono::steady_clock::now();
  metrics.recordSnapshot(now);
  metrics.recordSnapshot(now + 150ms);
  metrics.recordDurableAppend(17ms);
  lol::game_flow::RoomExecutionObservation room{
      .gameplayProgressTotal = 7,
      .serverInvariantTotal = 1,
      .latestQueueDelay = 4ms,
      .latestProcessingDuration = 2ms,
      .latestCriticalTerminalLatency = 9ms,
  };
  lol::settlement::SettlementCapacityMetrics settlement{
      .backlog =
          lol::settlement::OutboxBacklogSnapshot{
              .unretiredRecords = 3,
              .unretiredBytes = 30,
              .oldestPendingAge = 5ms,
              .storageHealthy = true,
          },
  };
  const auto snapshot = metrics.snapshot(room, settlement);
  return snapshot.roomQueueDelayMs == 4.0 &&
         snapshot.roomProcessingDurationMs == 2.0 &&
         snapshot.criticalTerminalLatencyMs == 9.0 &&
         snapshot.activeSnapshotIntervalMs == 150.0 &&
         snapshot.durableAppendLatencyMs == 17.0 &&
         snapshot.outboxBacklogRecords == 3.0 &&
         snapshot.outboxOldestPendingMs == 5.0 &&
         snapshot.gameplayProgressTotal == 7.0 &&
         snapshot.serverInvariantTotal == 1.0;
}

bool privateHttpContractIsReadOnlyBoundedAndDefaultDeny() {
  std::size_t snapshots{};
  PrivateMetricsServer server{PrivateMetricsServerConfig{
                                  .bindAddress = "localhost",
                                  .port = 0,
                                  .readToken = std::string(43, 'm'),
                                  .sourceIdentityDigest = std::string(64, 'a'),
                                  .requestTimeout = 1s,
                                  .maxRequestBytes = 64u * 1024u,
                                  .maxResponseBytes = 64u * 1024u,
                              },
                              [&snapshots] {
                                ++snapshots;
                                GameMetricSnapshot snapshot{};
                                snapshot.roomQueueDelayMs = 4.0;
                                snapshot.gameplayProgressTotal = 7.0;
                                snapshot.serverInvariantTotal = 0.0;
                                return snapshot;
                              }};
  if (!server.start() || server.port() == 0u) {
    return false;
  }
  const std::string authorization = "Bearer " + std::string(43, 'm');
  const auto ok = request(
      server.port(),
      "GET /private/metrics/v1 HTTP/1.1\r\nHost: game\r\nAuthorization: " +
          authorization + "\r\nConnection: close\r\n\r\n");
  const auto missing =
      request(server.port(), "GET /private/metrics/v1 HTTP/1.1\r\nHost: "
                             "game\r\nConnection: close\r\n\r\n");
  const auto wrongVersion = request(
      server.port(),
      "GET /private/metrics/v2 HTTP/1.1\r\nHost: game\r\nAuthorization: " +
          authorization + "\r\nConnection: close\r\n\r\n");
  const auto mutation = request(
      server.port(),
      "POST /private/metrics/v1 HTTP/1.1\r\nHost: game\r\nAuthorization: " +
          authorization + "\r\nContent-Length: 0\r\n\r\n");
  server.stop();

  return ok.starts_with("HTTP/1.1 200 ") &&
         ok.find("Cache-Control: no-store") != std::string::npos &&
         ok.find("\"schemaVersion\":1") != std::string::npos &&
         ok.find("\"source\":\"game\"") != std::string::npos &&
         ok.find("\"name\":\"room_queue_delay_ms\",\"value\":4") !=
             std::string::npos &&
         ok.size() <= 64u * 1024u && containsNoForbiddenIdentity(ok) &&
         missing.starts_with("HTTP/1.1 401 ") &&
         wrongVersion.starts_with("HTTP/1.1 404 ") &&
         mutation.starts_with("HTTP/1.1 405 ") && snapshots == 1u;
}

bool publicOrWildcardBindIsRejected() {
  for (const auto *address : {"0.0.0.0", "8.8.8.8"}) {
    try {
      PrivateMetricsServer server{
          PrivateMetricsServerConfig{
              .bindAddress = address,
              .port = 1,
              .readToken = std::string(43, 'm'),
              .sourceIdentityDigest = std::string(64, 'a'),
              .requestTimeout = 1s,
              .maxRequestBytes = 64u * 1024u,
              .maxResponseBytes = 64u * 1024u,
          },
          [] { return GameMetricSnapshot{}; }};
      static_cast<void>(server);
      return false;
    } catch (const std::invalid_argument &) {
    }
  }
  return true;
}

} // namespace

int main() {
  return gameMetricsPreserveOnlyBoundedAggregates() &&
                 privateHttpContractIsReadOnlyBoundedAndDefaultDeny() &&
                 publicOrWildcardBindIsRejected()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
