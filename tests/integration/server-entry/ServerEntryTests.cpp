#include <lol/battle_continuity/BattleReplay.hpp>
#include <lol/battle_continuity_storage/ContinuityStorage.hpp>
#include <lol/settlement_storage/JournalRecovery.hpp>
#include <lol/settlement_storage/SegmentJournal.hpp>
#include <lol/transport/rudp/RudpCodec.hpp>
#include <lol/transport/rudp/RudpCombatCodec.hpp>
#include <lol/transport/rudp/RudpHeader.hpp>
#include <lol/transport/rudp/RudpLootCodec.hpp>
#include <lol/transport/rudp/RudpMovementCodec.hpp>
#include <lol/transport/rudp/RudpPeer.hpp>
#include <lol/transport/tcp/BattleLoadProtocol.hpp>
#include <lol/transport/tcp/LobbyRoomProtocol.hpp>
#include <lol/transport/tcp/SessionProtocolCodec.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using lol::transport::rudp::AckState;
using lol::transport::rudp::ReceiveDisposition;
using lol::transport::rudp::RudpAttackApplied;
using lol::transport::rudp::RudpAttackIntent;
using lol::transport::rudp::RudpBindAccepted;
using lol::transport::rudp::RudpBindHello;
using lol::transport::rudp::RudpClaimLootIntent;
using lol::transport::rudp::RudpClaimLootResultCode;
using lol::transport::rudp::RudpClaimLootTerminalResult;
using lol::transport::rudp::RudpCombatCodec;
using lol::transport::rudp::RudpCombatMessage;
using lol::transport::rudp::RudpCommandId;
using lol::transport::rudp::RudpControlCodec;
using lol::transport::rudp::RudpControlCodecError;
using lol::transport::rudp::RudpControlMessage;
using lol::transport::rudp::RudpDropStateSnapshot;
using lol::transport::rudp::RudpFlag;
using lol::transport::rudp::RudpHeader;
using lol::transport::rudp::RudpHeaderCodec;
using lol::transport::rudp::RudpHeaderError;
using lol::transport::rudp::RudpHeartbeat;
using lol::transport::rudp::RudpLootCodec;
using lol::transport::rudp::RudpLootMessage;
using lol::transport::rudp::RudpMoveIntent;
using lol::transport::rudp::RudpMovementCodec;
using lol::transport::rudp::RudpMovementMessage;
using lol::transport::rudp::RudpPeerDelivery;
using lol::transport::rudp::RudpStateSnapshot;
using lol::transport::tcp::ArenaLoadComplete;
using lol::transport::tcp::AuthenticateGameSession;
using lol::transport::tcp::AuthenticationRejected;
using lol::transport::tcp::AuthenticationRejectedReason;
using lol::transport::tcp::BattleLoadClientMessage;
using lol::transport::tcp::BattleLoadProtocolCodec;
using lol::transport::tcp::BattleResumeSnapshot;
using lol::transport::tcp::BattleResumeSnapshotApplied;
using lol::transport::tcp::CreateRoom;
using lol::transport::tcp::HostStartRequest;
using lol::transport::tcp::JoinRoom;
using lol::transport::tcp::LeaveRoom;
using lol::transport::tcp::LobbyRoomClientMessage;
using lol::transport::tcp::LobbyRoomProtocolCodec;
using lol::transport::tcp::RequestRudpBindCapability;
using lol::transport::tcp::ResumeBattleSession;
using lol::transport::tcp::SessionProtocolCodec;
using lol::transport::tcp::SetReady;
using lol::transport::tcp::Welcome;

constexpr std::uint32_t kWelcomeMessageId = 2u;
constexpr std::uint32_t kAuthenticationRejectedMessageId = 3u;
constexpr std::uint32_t kSessionReplacedMessageId = 4u;
constexpr std::uint32_t kLobbyRoomListUpdateMessageId = 6u;
constexpr std::uint32_t kRoomCommandResponseMessageId = 12u;
constexpr std::uint32_t kRoomDetailMessageId = 13u;
constexpr std::uint32_t kBattleCommandResponseMessageId = 15u;
constexpr std::uint32_t kArenaLoadEntryMessageId = 16u;
constexpr std::uint32_t kArenaGameplayStartMessageId = 18u;
constexpr std::uint32_t kRudpBindCapabilityMessageId = 21u;
constexpr std::uint32_t kFinalResultMessageId = 36u;
constexpr std::uint32_t kBattleResumeSnapshotMessageId = 39u;

constexpr std::string_view kCredentialA =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
constexpr std::string_view kCredentialB =
    "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
constexpr std::string_view kMetricsCredential =
    "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";

using RudpCapability = lol::transport::rudp::RudpBindCapability;
using TcpRudpBindCapability = lol::transport::tcp::RudpBindCapability;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    const std::string value = "/tmp/lol-server-entry-XXXXXX";
    std::copy(value.begin(), value.end(), pattern.begin());
    char *created = ::mkdtemp(pattern.data());
    if (created != nullptr) {
      path_ = created;
      static_cast<void>(::chmod(path_.c_str(), S_IRWXU));
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }

private:
  std::filesystem::path path_;
};

bool writeText(const std::filesystem::path &path, std::string_view text,
               mode_t mode) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  return output.good() && ::chmod(path.c_str(), mode) == 0;
}

std::optional<std::vector<std::uint8_t>>
readBytes(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    return std::nullopt;
  }
  const auto end = input.tellg();
  if (end < 0) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return input.good() ? std::optional{std::move(bytes)} : std::nullopt;
}

std::string hashHex(const lol::battle_continuity::Hash &hash) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  text.reserve(hash.size() * 2u);
  for (const auto byte : hash) {
    text.push_back(digits[byte >> 4u]);
    text.push_back(digits[byte & 0x0fu]);
  }
  return text;
}

std::string configText(const TemporaryDirectory &directory,
                       std::uint16_t tcpPort = 0u, std::uint16_t udpPort = 0u,
                       std::optional<std::uint16_t> metricsPort = std::nullopt,
                       bool continuityEnabled = false) {
  std::string config =
      "bind_address=127.0.0.1\n"
      "tcp_port=" +
      std::to_string(tcpPort) + "\n" + "udp_port=" + std::to_string(udpPort) +
      "\n" + "journal_path=" + (directory.path() / "outbox.journal").string() +
      "\n" +
      "meta_claim_url=https://meta.test/internal/v1/game-credentials/claim\n"
      "meta_ca_certificate_file=/tmp/unused-meta-ca.pem\n"
      "meta_ca_certificate_sha256="
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
      "meta_expected_hostname=meta.test\n"
      "meta_settlements_url=https://meta.test/internal/v1/settlements\n"
      "meta_service_credential_file=" +
      (directory.path() / "meta.credential").string() + "\n" +
      (metricsPort.has_value()
           ? "metrics_enabled=true\nmetrics_bind_address=127.0.0.1\n"
             "metrics_port=" +
                 std::to_string(*metricsPort) +
                 "\nmetrics_read_credential_file=" +
                 (directory.path() / "metrics.credential").string() +
                 "\nmetrics_source_identity_digest=" + std::string(64u, 'a') +
                 "\nmetrics_allocated_cpu_count=2\n"
           : "metrics_enabled=false\n") +
      "worker_threads=2\n"
      "worker_queue_capacity=256\n"
      "deadline_capacity=128\n"
      "max_connections=16\n"
      "test_meta_fixture=true\n" +
      (continuityEnabled
           ? "battle_continuity_root=" +
                 (directory.path() / "battle-continuity").string() + "\n" +
                 "battle_continuity_key_file=" +
                 (directory.path() / "battle-continuity.key").string() + "\n"
           : "");
  return config;
}

std::optional<std::uint16_t> reserveTcpPort() {
  const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return std::nullopt;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  socklen_t addressBytes = sizeof(address);
  const bool ready =
      ::bind(descriptor, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) == 0 &&
      ::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address),
                    &addressBytes) == 0;
  const auto port =
      ready ? std::optional{ntohs(address.sin_port)} : std::nullopt;
  ::close(descriptor);
  return port;
}

class ChildProcess final {
public:
  ~ChildProcess() {
    if (processId_ > 0) {
      static_cast<void>(::kill(processId_, SIGKILL));
      int status = 0;
      static_cast<void>(::waitpid(processId_, &status, 0));
    }
    if (output_ >= 0) {
      ::close(output_);
    }
  }

  [[nodiscard]] bool start(const std::filesystem::path &config) {
    int pipeDescriptors[2]{};
    if (::pipe(pipeDescriptors) != 0) {
      return false;
    }
    processId_ = ::fork();
    if (processId_ == 0) {
      ::close(pipeDescriptors[0]);
      static_cast<void>(::dup2(pipeDescriptors[1], STDOUT_FILENO));
      static_cast<void>(::dup2(pipeDescriptors[1], STDERR_FILENO));
      ::close(pipeDescriptors[1]);
      const std::string configTextValue = config.string();
      ::execl(LOOT_GAME_SERVER_PATH, LOOT_GAME_SERVER_PATH, "--config",
              configTextValue.c_str(), static_cast<char *>(nullptr));
      _exit(127);
    }
    ::close(pipeDescriptors[1]);
    if (processId_ < 0) {
      ::close(pipeDescriptors[0]);
      processId_ = -1;
      return false;
    }
    output_ = pipeDescriptors[0];
    const int flags = ::fcntl(output_, F_GETFL, 0);
    return flags >= 0 && ::fcntl(output_, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  [[nodiscard]] std::optional<std::string>
  readLine(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto newline = bufferedOutput_.find('\n');
      if (newline != std::string::npos) {
        std::string line = bufferedOutput_.substr(0, newline);
        bufferedOutput_.erase(0, newline + 1u);
        return line;
      }
      pollfd descriptor{.fd = output_, .events = POLLIN, .revents = 0};
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      const int waitMillis = static_cast<int>(std::max<std::int64_t>(
          1, std::min<std::int64_t>(remaining.count(), 100)));
      const int ready = ::poll(&descriptor, 1, waitMillis);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        continue;
      }
      std::array<char, 512> bytes{};
      const auto read = ::read(output_, bytes.data(), bytes.size());
      if (read > 0) {
        bufferedOutput_.append(bytes.data(), static_cast<std::size_t>(read));
        continue;
      }
      if (read == 0) {
        return std::nullopt;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool running() const {
    return processId_ > 0 && ::kill(processId_, 0) == 0;
  }

  [[nodiscard]] std::optional<int> stop(int signal,
                                        std::chrono::milliseconds timeout) {
    if (processId_ <= 0 || ::kill(processId_, signal) != 0) {
      return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const auto waited = ::waitpid(processId_, &status, WNOHANG);
      if (waited == processId_) {
        processId_ = -1;
        if (WIFEXITED(status)) {
          return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
          return -WTERMSIG(status);
        }
        return std::nullopt;
      }
      if (waited < 0 && errno != EINTR) {
        return std::nullopt;
      }
      std::this_thread::sleep_for(10ms);
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<int> wait(std::chrono::milliseconds timeout) {
    if (processId_ <= 0) {
      return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const auto waited = ::waitpid(processId_, &status, WNOHANG);
      if (waited == processId_) {
        processId_ = -1;
        return WIFEXITED(status) ? std::optional{WEXITSTATUS(status)}
                                 : std::nullopt;
      }
      if (waited < 0 && errno != EINTR) {
        return std::nullopt;
      }
      std::this_thread::sleep_for(10ms);
    }
    return std::nullopt;
  }

private:
  pid_t processId_{-1};
  int output_{-1};
  std::string bufferedOutput_;
};

struct ReadyPorts final {
  std::uint16_t tcp{};
  std::uint16_t udp{};
};

std::optional<ReadyPorts> parseReady(std::string_view line) {
  constexpr std::string_view prefix = "READY tcp=";
  constexpr std::string_view middle = " udp=";
  if (!line.starts_with(prefix)) {
    return std::nullopt;
  }
  const auto separator = line.find(middle, prefix.size());
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  try {
    const auto tcp = std::stoul(
        std::string{line.substr(prefix.size(), separator - prefix.size())});
    const auto udp =
        std::stoul(std::string{line.substr(separator + middle.size())});
    if (tcp == 0u || tcp > 65535u || udp == 0u || udp > 65535u) {
      return std::nullopt;
    }
    return ReadyPorts{.tcp = static_cast<std::uint16_t>(tcp),
                      .udp = static_cast<std::uint16_t>(udp)};
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<ReadyPorts> readRecoveryReady(ChildProcess &child,
                                            std::uint32_t expectedEpoch) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  bool recoveringSeen = false;
  const std::string expected =
      "RECOVERING epoch=" + std::to_string(expectedEpoch) + " ";
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    const auto line = child.readLine(remaining);
    if (!line.has_value()) {
      return std::nullopt;
    }
    std::cerr << "continuity child: " << *line << '\n';
    if (line->starts_with(expected)) {
      recoveringSeen = true;
      continue;
    }
    if (auto ports = parseReady(*line); ports.has_value()) {
      return recoveringSeen ? ports : std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<int> connectTcp(std::uint16_t port) {
  const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return std::nullopt;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
    ::close(descriptor);
    return std::nullopt;
  }
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    ::close(descriptor);
    return std::nullopt;
  }
  return descriptor;
}

bool sendAll(int descriptor, std::span<const std::byte> bytes) {
  std::size_t sent{};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (sent < bytes.size() && std::chrono::steady_clock::now() < deadline) {
    const auto result =
        ::send(descriptor, bytes.data() + sent, bytes.size() - sent, 0);
    if (result > 0) {
      sent += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd writable{.fd = descriptor, .events = POLLOUT, .revents = 0};
      static_cast<void>(::poll(&writable, 1, 50));
      continue;
    }
    return false;
  }
  return sent == bytes.size();
}

std::string httpGetMetrics(std::uint16_t port) {
  auto descriptor = connectTcp(port);
  if (!descriptor.has_value()) {
    return {};
  }
  const std::string request =
      "GET /private/metrics/v1 HTTP/1.1\r\nHost: game\r\nAuthorization: "
      "Bearer " +
      std::string{kMetricsCredential} + "\r\nConnection: close\r\n\r\n";
  if (!sendAll(*descriptor, std::as_bytes(std::span{request}))) {
    ::close(*descriptor);
    return {};
  }
  std::string response;
  std::array<char, 4096> bytes{};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd readable{.fd = *descriptor, .events = POLLIN, .revents = 0};
    if (::poll(&readable, 1, 50) <= 0) {
      continue;
    }
    const auto received = ::recv(*descriptor, bytes.data(), bytes.size(), 0);
    if (received > 0) {
      response.append(bytes.data(), static_cast<std::size_t>(received));
      continue;
    }
    if (received == 0) {
      break;
    }
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      response.clear();
      break;
    }
  }
  ::close(*descriptor);
  return response;
}

std::uint32_t messageId(std::span<const std::byte> frame) {
  if (frame.size() < 9u) {
    return 0u;
  }
  std::uint32_t value{};
  for (std::size_t offset = 5u; offset < 9u; ++offset) {
    value = (value << 8u) | std::to_integer<std::uint32_t>(frame[offset]);
  }
  return value;
}

std::uint64_t readU64(std::span<const std::byte> frame, std::size_t offset) {
  if (frame.size() - std::min(frame.size(), offset) < 8u) {
    return 0u;
  }
  std::uint64_t value{};
  for (std::size_t index = 0; index < 8u; ++index) {
    value =
        (value << 8u) | std::to_integer<std::uint64_t>(frame[offset + index]);
  }
  return value;
}

std::uint16_t readU16(std::span<const std::byte> frame, std::size_t offset) {
  if (frame.size() - std::min(frame.size(), offset) < 2u) {
    return std::numeric_limits<std::uint16_t>::max();
  }
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint16_t>(frame[offset]) << 8u) |
      std::to_integer<std::uint16_t>(frame[offset + 1u]));
}

class TcpClient final {
public:
  explicit TcpClient(int descriptor) : descriptor_(descriptor) {}
  ~TcpClient() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }

  TcpClient(const TcpClient &) = delete;
  TcpClient &operator=(const TcpClient &) = delete;

  [[nodiscard]] bool send(std::span<const std::byte> frame) const {
    return sendAll(descriptor_, frame);
  }

  void closeNow() noexcept {
    if (descriptor_ >= 0) {
      ::shutdown(descriptor_, SHUT_RDWR);
      ::close(descriptor_);
      descriptor_ = -1;
    }
  }

  void pump(std::chrono::milliseconds timeout) {
    pollfd readable{.fd = descriptor_, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&readable, 1, static_cast<int>(timeout.count()));
    if (ready <= 0 || (readable.revents & POLLIN) == 0) {
      return;
    }
    std::array<std::byte, 4096> chunk{};
    while (true) {
      const auto received = ::recv(descriptor_, chunk.data(), chunk.size(), 0);
      if (received > 0) {
        inbound_.insert(inbound_.end(), chunk.begin(),
                        chunk.begin() + received);
      } else if (received < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
    extractFrames();
  }

  [[nodiscard]] std::optional<std::vector<std::byte>>
  take(std::uint32_t expected, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto found =
          std::ranges::find_if(frames_, [expected](const auto &f) {
            return messageId(f) == expected;
          });
      if (found != frames_.end()) {
        auto frame = std::move(*found);
        frames_.erase(found);
        return frame;
      }
      pump(20ms);
    }
    return std::nullopt;
  }

  [[nodiscard]] bool
  waitForClose(std::chrono::milliseconds timeout) const noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<std::byte, 4096> bytes{};
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd readable{.fd = descriptor_, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&readable, 1, 50);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        continue;
      }
      if ((readable.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        return true;
      }
      if ((readable.revents & POLLIN) != 0) {
        const auto received =
            ::recv(descriptor_, bytes.data(), bytes.size(), 0);
        if (received == 0) {
          return true;
        }
        if (received < 0 && errno != EINTR && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
          return true;
        }
      }
    }
    return false;
  }

private:
  void extractFrames() {
    while (inbound_.size() >= 4u) {
      std::uint32_t payloadBytes{};
      for (std::size_t index = 0; index < 4u; ++index) {
        payloadBytes = (payloadBytes << 8u) |
                       std::to_integer<std::uint32_t>(inbound_[index]);
      }
      const std::size_t frameBytes = 4u + payloadBytes;
      if (payloadBytes == 0u || frameBytes > 65540u ||
          inbound_.size() < frameBytes) {
        return;
      }
      frames_.emplace_back(inbound_.begin(),
                           inbound_.begin() +
                               static_cast<std::ptrdiff_t>(frameBytes));
      inbound_.erase(inbound_.begin(),
                     inbound_.begin() +
                         static_cast<std::ptrdiff_t>(frameBytes));
    }
  }

  int descriptor_;
  std::vector<std::byte> inbound_;
  std::vector<std::vector<std::byte>> frames_;
};

std::optional<Welcome> authenticate(TcpClient &client,
                                    std::string_view credential,
                                    std::uint64_t requestId) {
  const auto frame = SessionProtocolCodec::encodeFrame(
      lol::transport::tcp::SessionControlMessage{AuthenticateGameSession{
          .requestId = requestId, .credential = std::string{credential}}});
  if (!frame.has_value() || !client.send(*frame)) {
    return std::nullopt;
  }
  const auto welcomeFrame = client.take(kWelcomeMessageId, 3s);
  if (!welcomeFrame.has_value()) {
    return std::nullopt;
  }
  const auto decoded = SessionProtocolCodec::decodeFrame(*welcomeFrame);
  if (!decoded.message.has_value()) {
    return std::nullopt;
  }
  const auto *welcome = std::get_if<Welcome>(&*decoded.message);
  return welcome == nullptr ? std::nullopt : std::optional{*welcome};
}

struct ResumedConnection final {
  Welcome welcome;
  BattleResumeSnapshot snapshot;
};

std::optional<ResumedConnection> resumeBattle(TcpClient &client,
                                              const Welcome &previous,
                                              std::string credential,
                                              std::uint64_t requestId) {
  const auto frame = SessionProtocolCodec::encodeFrame(ResumeBattleSession{
      .requestId = requestId,
      .previousSessionId = previous.sessionId,
      .previousSessionGeneration = previous.sessionGeneration,
      .credential = std::move(credential),
  });
  if (!frame.has_value() || !client.send(*frame)) {
    return std::nullopt;
  }
  const auto welcomeFrame = client.take(kWelcomeMessageId, 3s);
  const auto snapshotFrame = client.take(kBattleResumeSnapshotMessageId, 3s);
  if (!welcomeFrame.has_value() || !snapshotFrame.has_value()) {
    return std::nullopt;
  }
  const auto decodedWelcome = SessionProtocolCodec::decodeFrame(*welcomeFrame);
  const auto decodedSnapshot =
      SessionProtocolCodec::decodeFrame(*snapshotFrame);
  const auto *welcome = decodedWelcome.message.has_value()
                            ? std::get_if<Welcome>(&*decodedWelcome.message)
                            : nullptr;
  const auto *snapshot =
      decodedSnapshot.message.has_value()
          ? std::get_if<BattleResumeSnapshot>(&*decodedSnapshot.message)
          : nullptr;
  if (welcome == nullptr || snapshot == nullptr) {
    return std::nullopt;
  }
  return ResumedConnection{.welcome = *welcome, .snapshot = *snapshot};
}

struct UdpClient final {
  int descriptor{-1};
  sockaddr_in server{};
  std::uint64_t sessionId{};
  std::uint64_t generation{};
  std::uint32_t transportEpoch{};
  std::uint32_t nextSequence{2u};
  RudpPeerDelivery serverDelivery;

  ~UdpClient() {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
  }
  UdpClient() = default;
  UdpClient(const UdpClient &) = delete;
  UdpClient &operator=(const UdpClient &) = delete;
};

std::optional<RudpCapability> requestCapability(TcpClient &client,
                                                std::uint64_t requestId) {
  const auto request = SessionProtocolCodec::encodeFrame(
      lol::transport::tcp::SessionControlMessage{
          RequestRudpBindCapability{.requestId = requestId}});
  if (!request.has_value() || !client.send(*request)) {
    return std::nullopt;
  }
  const auto frame = client.take(kRudpBindCapabilityMessageId, 2s);
  if (!frame.has_value()) {
    return std::nullopt;
  }
  const auto decoded = SessionProtocolCodec::decodeFrame(*frame);
  if (!decoded.message.has_value()) {
    return std::nullopt;
  }
  const auto *capability =
      std::get_if<TcpRudpBindCapability>(&*decoded.message);
  if (capability == nullptr) {
    return std::nullopt;
  }
  return RudpCapability{capability->capability};
}

bool bindUdp(UdpClient &client, const ReadyPorts &ports, const Welcome &welcome,
             const RudpCapability &capability) {
  client.descriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (client.descriptor < 0) {
    return false;
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  local.sin_port = 0;
  if (::bind(client.descriptor, reinterpret_cast<const sockaddr *>(&local),
             sizeof(local)) != 0) {
    return false;
  }
  client.server.sin_family = AF_INET;
  client.server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  client.server.sin_port = htons(ports.udp);
  client.sessionId = welcome.sessionId;
  client.generation = welcome.sessionGeneration;
  const auto hello = RudpControlCodec::encode(
      RudpHeader{.flag = RudpFlag::Reliable,
                 .sessionId = client.sessionId,
                 .sessionGeneration = client.generation,
                 .transportEpoch = 0u,
                 .sequence = 1u,
                 .ack = 0u,
                 .ackBits = 0u,
                 .messageId = 22u},
      RudpControlMessage{RudpBindHello{capability}});
  if (!hello.has_value() ||
      ::sendto(client.descriptor, hello->data(), hello->size(), 0,
               reinterpret_cast<const sockaddr *>(&client.server),
               sizeof(client.server)) != static_cast<ssize_t>(hello->size())) {
    return false;
  }
  pollfd readable{.fd = client.descriptor, .events = POLLIN, .revents = 0};
  if (::poll(&readable, 1, 2000) != 1) {
    return false;
  }
  std::array<std::byte, 2048> datagram{};
  const auto received =
      ::recv(client.descriptor, datagram.data(), datagram.size(), 0);
  if (received <= 0) {
    return false;
  }
  const auto decoded = RudpControlCodec::decode(
      std::span{datagram}.first(static_cast<std::size_t>(received)));
  if (decoded.error != RudpControlCodecError::None ||
      !decoded.header.has_value() || !decoded.message.has_value() ||
      !std::holds_alternative<RudpBindAccepted>(*decoded.message)) {
    return false;
  }
  client.transportEpoch = decoded.header->transportEpoch;
  static_cast<void>(client.serverDelivery.observe(decoded.header->sequence));
  const int flags = ::fcntl(client.descriptor, F_GETFL, 0);
  return client.transportEpoch != 0u && flags >= 0 &&
         ::fcntl(client.descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

void drainUdp(UdpClient &client) {
  std::array<std::byte, 65535> datagram{};
  while (true) {
    const auto received =
        ::recv(client.descriptor, datagram.data(), datagram.size(), 0);
    if (received > 0) {
      const auto header = RudpHeaderCodec::decode(
          std::span{datagram}.first(static_cast<std::size_t>(received)));
      if (header.error == RudpHeaderError::None && header.header.has_value()) {
        static_cast<void>(
            client.serverDelivery.observe(header.header->sequence));
      }
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

void discardUdpWithoutAck(UdpClient &client) {
  std::array<std::byte, 65535> datagram{};
  while (true) {
    const auto received =
        ::recv(client.descriptor, datagram.data(), datagram.size(), 0);
    if (received > 0) {
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

bool heartbeat(UdpClient &client) {
  drainUdp(client);
  const AckState ack = client.serverDelivery.ackState();
  const auto datagram = RudpControlCodec::encode(
      RudpHeader{.flag = RudpFlag::Heartbeat,
                 .sessionId = client.sessionId,
                 .sessionGeneration = client.generation,
                 .transportEpoch = client.transportEpoch,
                 .sequence = client.nextSequence++,
                 .ack = ack.ack,
                 .ackBits = ack.ackBits,
                 .messageId = 24u},
      RudpControlMessage{RudpHeartbeat{}});
  return datagram.has_value() &&
         ::sendto(client.descriptor, datagram->data(), datagram->size(), 0,
                  reinterpret_cast<const sockaddr *>(&client.server),
                  sizeof(client.server)) ==
             static_cast<ssize_t>(datagram->size());
}

bool heartbeatWithoutOutboundAck(UdpClient &client) {
  discardUdpWithoutAck(client);
  const auto datagram = RudpControlCodec::encode(
      RudpHeader{.flag = RudpFlag::Heartbeat,
                 .sessionId = client.sessionId,
                 .sessionGeneration = client.generation,
                 .transportEpoch = client.transportEpoch,
                 .sequence = client.nextSequence++,
                 .ack = 1u,
                 .ackBits = 0u,
                 .messageId = 24u},
      RudpControlMessage{RudpHeartbeat{}});
  return datagram.has_value() &&
         ::sendto(client.descriptor, datagram->data(), datagram->size(), 0,
                  reinterpret_cast<const sockaddr *>(&client.server),
                  sizeof(client.server)) ==
             static_cast<ssize_t>(datagram->size());
}

bool attackWithoutOutboundAck(UdpClient &client, std::uint64_t commandId) {
  const auto datagram = RudpCombatCodec::encode(
      RudpHeader{.flag = RudpFlag::Reliable,
                 .sessionId = client.sessionId,
                 .sessionGeneration = client.generation,
                 .transportEpoch = client.transportEpoch,
                 .sequence = client.nextSequence++,
                 .ack = 1u,
                 .ackBits = 0u,
                 .messageId = 27u},
      RudpCombatMessage{RudpAttackIntent{
          .commandId = RudpCommandId{.high = 0u, .low = commandId},
          .battleInstanceId = 1u,
          .targetHint = 1u,
      }});
  return datagram.has_value() &&
         ::sendto(client.descriptor, datagram->data(), datagram->size(), 0,
                  reinterpret_cast<const sockaddr *>(&client.server),
                  sizeof(client.server)) ==
             static_cast<ssize_t>(datagram->size());
}

bool sendMove(UdpClient &client, std::uint32_t actionSequence, std::int16_t x,
              std::int16_t y, std::uint64_t battleInstanceId = 1u) {
  drainUdp(client);
  const auto ack = client.serverDelivery.ackState();
  const auto datagram = RudpMovementCodec::encode(
      RudpHeader{.flag = RudpFlag::Unreliable,
                 .sessionId = client.sessionId,
                 .sessionGeneration = client.generation,
                 .transportEpoch = client.transportEpoch,
                 .sequence = client.nextSequence++,
                 .ack = ack.ack,
                 .ackBits = ack.ackBits,
                 .messageId = 25u},
      RudpMovementMessage{RudpMoveIntent{.battleInstanceId = battleInstanceId,
                                         .actionSequence = actionSequence,
                                         .desiredX = x,
                                         .desiredY = y,
                                         .inputFlags = 0u}});
  return datagram.has_value() &&
         ::sendto(client.descriptor, datagram->data(), datagram->size(), 0,
                  reinterpret_cast<const sockaddr *>(&client.server),
                  sizeof(client.server)) ==
             static_cast<ssize_t>(datagram->size());
}

bool sendAttack(UdpClient &client, std::uint64_t commandId,
                std::uint64_t battleInstanceId = 1u) {
  drainUdp(client);
  const auto ack = client.serverDelivery.ackState();
  const auto datagram = RudpCombatCodec::encode(
      RudpHeader{.flag = RudpFlag::Reliable,
                 .sessionId = client.sessionId,
                 .sessionGeneration = client.generation,
                 .transportEpoch = client.transportEpoch,
                 .sequence = client.nextSequence++,
                 .ack = ack.ack,
                 .ackBits = ack.ackBits,
                 .messageId = 27u},
      RudpCombatMessage{RudpAttackIntent{
          .commandId = RudpCommandId{.high = 0u, .low = commandId},
          .battleInstanceId = battleInstanceId,
          .targetHint = 1u,
      }});
  return datagram.has_value() &&
         ::sendto(client.descriptor, datagram->data(), datagram->size(), 0,
                  reinterpret_cast<const sockaddr *>(&client.server),
                  sizeof(client.server)) ==
             static_cast<ssize_t>(datagram->size());
}

bool sendClaimLoot(UdpClient &client, std::uint64_t commandId,
                   std::uint64_t dropId) {
  drainUdp(client);
  const auto ack = client.serverDelivery.ackState();
  const auto datagram = RudpLootCodec::encode(
      RudpHeader{.flag = RudpFlag::Reliable,
                 .sessionId = client.sessionId,
                 .sessionGeneration = client.generation,
                 .transportEpoch = client.transportEpoch,
                 .sequence = client.nextSequence++,
                 .ack = ack.ack,
                 .ackBits = ack.ackBits,
                 .messageId = 32u},
      RudpLootMessage{RudpClaimLootIntent{
          .commandId = RudpCommandId{.high = 0u, .low = commandId},
          .battleInstanceId = 1u,
          .dropId = dropId,
      }});
  return datagram.has_value() &&
         ::sendto(client.descriptor, datagram->data(), datagram->size(), 0,
                  reinterpret_cast<const sockaddr *>(&client.server),
                  sizeof(client.server)) ==
             static_cast<ssize_t>(datagram->size());
}

template <class Match>
bool receiveUdpUntil(UdpClient &client, std::chrono::milliseconds timeout,
                     Match match) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd readable{.fd = client.descriptor, .events = POLLIN, .revents = 0};
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    const int waitMillis = static_cast<int>(std::max<std::int64_t>(
        1, std::min<std::int64_t>(remaining.count(), 100)));
    const int ready = ::poll(&readable, 1, waitMillis);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0) {
      continue;
    }
    std::array<std::byte, 65535> datagram{};
    const auto received =
        ::recv(client.descriptor, datagram.data(), datagram.size(), 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received <= 0) {
      return false;
    }
    const auto bytes =
        std::span{datagram}.first(static_cast<std::size_t>(received));
    const auto header = RudpHeaderCodec::decode(bytes);
    if (header.error == RudpHeaderError::None && header.header.has_value()) {
      static_cast<void>(client.serverDelivery.observe(header.header->sequence));
    }
    if (match(bytes)) {
      return true;
    }
  }
  return false;
}

std::optional<RudpAttackApplied>
waitForAttackApplied(UdpClient &client, std::uint32_t remainingHitPoints,
                     std::chrono::milliseconds timeout = 3s) {
  std::optional<RudpAttackApplied> found;
  static_cast<void>(receiveUdpUntil(client, timeout, [&](const auto bytes) {
    const auto decoded = RudpCombatCodec::decode(bytes);
    const auto *applied =
        decoded.message.has_value()
            ? std::get_if<RudpAttackApplied>(&*decoded.message)
            : nullptr;
    if (applied == nullptr ||
        applied->remainingHitPoints != remainingHitPoints) {
      return false;
    }
    found = *applied;
    return true;
  }));
  return found;
}

template <class Match>
std::optional<RudpStateSnapshot>
waitForMovementSnapshot(UdpClient &client, Match match,
                        std::chrono::milliseconds timeout = 3s) {
  std::optional<RudpStateSnapshot> found;
  static_cast<void>(receiveUdpUntil(client, timeout, [&](const auto bytes) {
    const auto decoded = RudpMovementCodec::decode(bytes);
    const auto *snapshot =
        decoded.message.has_value()
            ? std::get_if<RudpStateSnapshot>(&*decoded.message)
            : nullptr;
    if (snapshot == nullptr || !match(*snapshot)) {
      return false;
    }
    found = *snapshot;
    return true;
  }));
  return found;
}

std::optional<RudpDropStateSnapshot>
waitForDrops(UdpClient &client, std::chrono::milliseconds timeout = 3s) {
  std::optional<RudpDropStateSnapshot> found;
  static_cast<void>(receiveUdpUntil(client, timeout, [&](const auto bytes) {
    const auto decoded = RudpLootCodec::decode(bytes);
    const auto *snapshot =
        decoded.message.has_value()
            ? std::get_if<RudpDropStateSnapshot>(&*decoded.message)
            : nullptr;
    if (snapshot == nullptr || snapshot->drops.size() != 2u) {
      return false;
    }
    found = *snapshot;
    return true;
  }));
  return found;
}

bool waitForClaimAccepted(UdpClient &client, std::uint64_t dropId,
                          std::chrono::milliseconds timeout = 3s) {
  return receiveUdpUntil(client, timeout, [&](const auto bytes) {
    const auto decoded = RudpLootCodec::decode(bytes);
    const auto *result =
        decoded.message.has_value()
            ? std::get_if<RudpClaimLootTerminalResult>(&*decoded.message)
            : nullptr;
    return result != nullptr && result->dropId == dropId &&
           result->resultCode == RudpClaimLootResultCode::Ok;
  });
}

template <class Message> bool sendLobby(TcpClient &client, Message message) {
  const auto frame = LobbyRoomProtocolCodec::encodeClientFrame(
      LobbyRoomClientMessage{std::move(message)});
  return frame.has_value() && client.send(*frame);
}

template <class Message> bool sendBattle(TcpClient &client, Message message) {
  const auto frame = BattleLoadProtocolCodec::encodeClientFrame(
      BattleLoadClientMessage{std::move(message)});
  return frame.has_value() && client.send(*frame);
}

std::optional<std::uint64_t>
createRoom(TcpClient &host, std::uint64_t requestId, std::string title) {
  if (!sendLobby(host, CreateRoom{.requestId = requestId,
                                  .title = std::move(title),
                                  .capacity = 2u}) ||
      !host.take(kRoomCommandResponseMessageId, 2s).has_value()) {
    return std::nullopt;
  }
  const auto detail = host.take(kRoomDetailMessageId, 2s);
  const auto roomId = detail.has_value() ? readU64(*detail, 9u) : 0u;
  return roomId == 0u ? std::nullopt : std::optional{roomId};
}

bool waitForCycleCompletion(TcpClient &host, TcpClient &member,
                            UdpClient &hostUdp, UdpClient &memberUdp) {
  bool finalSeen = false;
  const auto deadline = std::chrono::steady_clock::now() + 35s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!heartbeat(hostUdp) || !heartbeat(memberUdp)) {
      return false;
    }
    host.pump(20ms);
    member.pump(20ms);
    if (!finalSeen) {
      finalSeen = host.take(kFinalResultMessageId, 1ms).has_value();
    }
    if (finalSeen && host.take(kRoomDetailMessageId, 1ms).has_value()) {
      return true;
    }
    std::this_thread::sleep_for(400ms);
  }
  return false;
}

bool startBattle(TcpClient &host, TcpClient &member, std::uint64_t roomId,
                 std::uint64_t battleId, std::uint64_t requestBase) {
  if (!sendLobby(host, SetReady{.requestId = requestBase, .ready = true}) ||
      !host.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !sendLobby(member,
                 SetReady{.requestId = requestBase + 1u, .ready = true}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !sendBattle(host, HostStartRequest{.requestId = requestBase + 2u})) {
    return false;
  }
  const auto startResponse = host.take(kBattleCommandResponseMessageId, 2s);
  if (!startResponse.has_value() || readU16(*startResponse, 17u) != 0u) {
    return false;
  }
  const auto entry = host.take(kArenaLoadEntryMessageId, 2s);
  if (!entry.has_value() || readU64(*entry, 9u) != roomId ||
      readU64(*entry, 17u) != battleId ||
      !sendBattle(host, ArenaLoadComplete{.requestId = requestBase + 3u,
                                          .roomId = roomId,
                                          .battleInstanceId = battleId}) ||
      !host.take(kBattleCommandResponseMessageId, 2s).has_value() ||
      !sendBattle(member, ArenaLoadComplete{.requestId = requestBase + 4u,
                                            .roomId = roomId,
                                            .battleInstanceId = battleId}) ||
      !member.take(kBattleCommandResponseMessageId, 2s).has_value() ||
      !host.take(kArenaGameplayStartMessageId, 2s).has_value()) {
    return false;
  }
  return true;
}

bool runBattleCycle(TcpClient &host, TcpClient &member, UdpClient &hostUdp,
                    UdpClient &memberUdp, std::uint64_t roomId,
                    std::uint64_t battleId, std::uint64_t requestBase) {
  if (!startBattle(host, member, roomId, battleId, requestBase)) {
    return false;
  }
  return waitForCycleCompletion(host, member, hostUdp, memberUdp);
}

bool invalidConfigFailsBeforeListener() {
  TemporaryDirectory directory;
  const auto port = reserveTcpPort();
  if (!directory.valid() || !port.has_value() ||
      !writeText(directory.path() / "meta.credential",
                 "fixture-service-credential", S_IRUSR | S_IWUSR)) {
    return false;
  }
  const auto config = directory.path() / "invalid.conf";
  const std::string invalid = "bind_address=127.0.0.1\n" +
                              std::string{"tcp_port="} + std::to_string(*port) +
                              "\n" + "udp_port=0\n";
  if (!writeText(config, invalid, S_IRUSR | S_IWUSR)) {
    return false;
  }
  ChildProcess child;
  if (!child.start(config)) {
    return false;
  }
  const auto exitCode = child.wait(3s);
  if (!exitCode.has_value() || *exitCode == 0) {
    return false;
  }
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(*port);
  const bool available =
      ::bind(listener, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) == 0;
  ::close(listener);
  return available;
}

bool productionEntryServesPrivateMetrics() {
  TemporaryDirectory directory;
  const auto metricsPort = reserveTcpPort();
  if (!directory.valid() || !metricsPort.has_value() ||
      !writeText(directory.path() / "meta.credential",
                 "fixture-service-credential", S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "metrics.credential", kMetricsCredential,
                 S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "server.conf",
                 configText(directory, 0u, 0u, metricsPort),
                 S_IRUSR | S_IWUSR)) {
    return false;
  }
  ChildProcess child;
  if (!child.start(directory.path() / "server.conf")) {
    return false;
  }
  const auto line = child.readLine(5s);
  if (!line.has_value() || !parseReady(*line).has_value()) {
    return false;
  }
  const auto response = httpGetMetrics(*metricsPort);
  const auto exitCode = child.stop(SIGTERM, 8s);
  return response.starts_with("HTTP/1.1 200 ") &&
         response.find("Cache-Control: no-store") != std::string::npos &&
         response.find("\"schemaVersion\":1") != std::string::npos &&
         response.find("\"source\":\"game\"") != std::string::npos &&
         response.find("\"sourceIdentityDigest\":\"" + std::string(64u, 'a') +
                       "\"") != std::string::npos &&
         exitCode.has_value() && *exitCode == 0;
}

template <class PeerDriver>
bool productionEntryObservesPeerClosure(std::string_view roomTitle,
                                        std::chrono::milliseconds closeTimeout,
                                        bool expectedHostClosed,
                                        PeerDriver drivePeer) {
  TemporaryDirectory directory;
  const auto continuityRoot = directory.path() / "battle-continuity";
  std::error_code directoryError;
  if (!directory.valid() ||
      !std::filesystem::create_directory(continuityRoot, directoryError) ||
      directoryError || ::chmod(continuityRoot.c_str(), S_IRWXU) != 0 ||
      !writeText(directory.path() / "battle-continuity.key",
                 std::string(32u, 'P'), S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "meta.credential",
                 "fixture-service-credential", S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "server.conf",
                 configText(directory, 0u, 0u, std::nullopt, true),
                 S_IRUSR | S_IWUSR)) {
    return false;
  }
  ChildProcess child;
  if (!child.start(directory.path() / "server.conf")) {
    return false;
  }
  const auto ports = readRecoveryReady(child, 1u);
  if (!ports.has_value() || !child.running()) {
    return false;
  }
  auto hostDescriptor = connectTcp(ports->tcp);
  auto memberDescriptor = connectTcp(ports->tcp);
  if (!hostDescriptor.has_value() || !memberDescriptor.has_value()) {
    if (hostDescriptor.has_value()) {
      ::close(*hostDescriptor);
    }
    if (memberDescriptor.has_value()) {
      ::close(*memberDescriptor);
    }
    return false;
  }
  TcpClient host{*hostDescriptor};
  TcpClient member{*memberDescriptor};
  const auto hostWelcome = authenticate(host, kCredentialA, 1u);
  const auto memberWelcome = authenticate(member, kCredentialB, 2u);
  const auto hostCapability = requestCapability(host, 3u);
  const auto memberCapability = requestCapability(member, 4u);
  UdpClient hostUdp;
  UdpClient memberUdp;
  if (!hostWelcome.has_value() || !memberWelcome.has_value() ||
      !hostCapability.has_value() || !memberCapability.has_value() ||
      !bindUdp(hostUdp, *ports, *hostWelcome, *hostCapability) ||
      !bindUdp(memberUdp, *ports, *memberWelcome, *memberCapability)) {
    return false;
  }
  const auto roomId = createRoom(host, 5u, std::string{roomTitle});
  if (!roomId.has_value() || (*roomId >> 32u) == 0u ||
      !sendLobby(member, JoinRoom{.requestId = 6u, .roomId = *roomId}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kRoomDetailMessageId, 2s).has_value() ||
      !startBattle(host, member, *roomId, 1u, 10u)) {
    return false;
  }
  if (!drivePeer(hostUdp, memberUdp)) {
    return false;
  }
  const bool hostClosed = host.waitForClose(closeTimeout);
  const bool processAlive = child.running();
  const bool memberContinues =
      sendLobby(member, LeaveRoom{.requestId = 100u}) &&
      member.take(kRoomCommandResponseMessageId, 2s).has_value();
  const auto exitCode = child.stop(SIGTERM, 8s);
  return hostClosed == expectedHostClosed && processAlive && memberContinues &&
         exitCode.has_value() && *exitCode == 0;
}

bool productionEntryClosesOnlyExpiredReliablePeer() {
  return productionEntryObservesPeerClosure(
      "expiry-room", 2s, true, [](UdpClient &host, UdpClient &member) {
        const auto deadline = std::chrono::steady_clock::now() + 6200ms;
        while (std::chrono::steady_clock::now() < deadline) {
          if (!heartbeatWithoutOutboundAck(host) || !heartbeat(member)) {
            return false;
          }
          std::this_thread::sleep_for(250ms);
        }
        return true;
      });
}

bool productionEntryKeepsRunningAfterPeerReliableQueuePressure() {
  return productionEntryObservesPeerClosure(
      "pressure-room", 3s, true, [](UdpClient &host, UdpClient &member) {
        for (std::uint64_t commandId = 1u; commandId <= 260u; ++commandId) {
          if (!attackWithoutOutboundAck(host, commandId)) {
            return false;
          }
          if (commandId % 20u == 0u) {
            discardUdpWithoutAck(host);
            if (!heartbeat(member)) {
              return false;
            }
          }
          std::this_thread::sleep_for(4ms);
        }
        return true;
      });
}

bool productionEntryAcceptsReliableAckPiggybackedOnMove() {
  return productionEntryObservesPeerClosure(
      "move-ack-room", 500ms, false, [](UdpClient &host, UdpClient &member) {
        if (!heartbeat(host) || !heartbeat(member)) {
          std::cerr << "movement ack setup heartbeat failed\n";
          return false;
        }
        if (!sendAttack(host, 700u)) {
          std::cerr << "movement ack attack send failed\n";
          return false;
        }
        if (!receiveUdpUntil(host, 3s, [](const auto bytes) {
              const auto decoded = RudpHeaderCodec::decode(bytes);
              return decoded.error == RudpHeaderError::None &&
                     decoded.header.has_value() &&
                     decoded.header->flag == RudpFlag::Reliable;
            })) {
          std::cerr << "movement ack reliable response missing\n";
          return false;
        }
        if (!sendMove(host, 1u, 0, 0)) {
          std::cerr << "movement ack move send failed\n";
          return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + 6200ms;
        while (std::chrono::steady_clock::now() < deadline) {
          if (!heartbeatWithoutOutboundAck(host) || !heartbeat(member)) {
            return false;
          }
          std::this_thread::sleep_for(250ms);
        }
        return true;
      });
}

bool productionEntrySupportsAuthTwoCyclesAndGracefulStop() {
  TemporaryDirectory directory;
  if (!directory.valid() ||
      !writeText(directory.path() / "meta.credential",
                 "fixture-service-credential", S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "server.conf", configText(directory),
                 S_IRUSR | S_IWUSR)) {
    return false;
  }
  ChildProcess child;
  if (!child.start(directory.path() / "server.conf")) {
    return false;
  }
  const auto line = child.readLine(5s);
  const auto ports = line.has_value() ? parseReady(*line) : std::nullopt;
  if (!ports.has_value() || !child.running()) {
    return false;
  }
  auto replacedHostDescriptor = connectTcp(ports->tcp);
  auto hostDescriptor = connectTcp(ports->tcp);
  auto memberDescriptor = connectTcp(ports->tcp);
  if (!replacedHostDescriptor.has_value() || !hostDescriptor.has_value() ||
      !memberDescriptor.has_value()) {
    if (replacedHostDescriptor.has_value()) {
      ::close(*replacedHostDescriptor);
    }
    if (hostDescriptor.has_value()) {
      ::close(*hostDescriptor);
    }
    if (memberDescriptor.has_value()) {
      ::close(*memberDescriptor);
    }
    return false;
  }
  TcpClient replacedHost{*replacedHostDescriptor};
  TcpClient host{*hostDescriptor};
  TcpClient member{*memberDescriptor};
  const auto replacedWelcome = authenticate(replacedHost, kCredentialA, 1u);
  const auto hostWelcome = authenticate(host, kCredentialA, 2u);
  const auto memberWelcome = authenticate(member, kCredentialB, 3u);
  if (!replacedWelcome.has_value() || !hostWelcome.has_value() ||
      !memberWelcome.has_value() ||
      hostWelcome->sessionGeneration <= replacedWelcome->sessionGeneration ||
      !replacedHost.take(kSessionReplacedMessageId, 2s).has_value()) {
    return false;
  }
  const auto hostCapability = requestCapability(host, 4u);
  const auto memberCapability = requestCapability(member, 5u);
  UdpClient hostUdp;
  UdpClient memberUdp;
  if (!hostCapability.has_value() || !memberCapability.has_value() ||
      !bindUdp(hostUdp, *ports, *hostWelcome, *hostCapability) ||
      !bindUdp(memberUdp, *ports, *memberWelcome, *memberCapability)) {
    return false;
  }
  const auto roomId = createRoom(host, 6u, "entry-room");
  if (!roomId.has_value() ||
      !member.take(kLobbyRoomListUpdateMessageId, 2s).has_value() ||
      !sendLobby(member, JoinRoom{.requestId = 7u, .roomId = *roomId}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kRoomDetailMessageId, 2s).has_value() ||
      !runBattleCycle(host, member, hostUdp, memberUdp, *roomId, 1u, 10u) ||
      !runBattleCycle(host, member, hostUdp, memberUdp, *roomId, 2u, 20u) ||
      !sendLobby(host, LeaveRoom{.requestId = 30u}) ||
      !host.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !sendLobby(member, LeaveRoom{.requestId = 31u}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kLobbyRoomListUpdateMessageId, 2s).has_value()) {
    return false;
  }
  const auto exitCode = child.stop(SIGTERM, 8s);
  if (!exitCode.has_value() || *exitCode != 0) {
    return false;
  }
  auto afterStop = connectTcp(ports->tcp);
  if (afterStop.has_value()) {
    ::close(*afterStop);
    return false;
  }
  return true;
}

bool productionEntryResumesSameSessionRoomAndBattleWithinGrace() {
  TemporaryDirectory directory;
  if (!directory.valid() ||
      !writeText(directory.path() / "meta.credential",
                 "fixture-service-credential", S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "server.conf", configText(directory),
                 S_IRUSR | S_IWUSR)) {
    return false;
  }
  ChildProcess child;
  if (!child.start(directory.path() / "server.conf")) {
    return false;
  }
  const auto line = child.readLine(5s);
  const auto ports = line.has_value() ? parseReady(*line) : std::nullopt;
  auto hostDescriptor =
      ports.has_value() ? connectTcp(ports->tcp) : std::nullopt;
  auto memberDescriptor =
      ports.has_value() ? connectTcp(ports->tcp) : std::nullopt;
  if (!ports.has_value() || !hostDescriptor.has_value() ||
      !memberDescriptor.has_value()) {
    return false;
  }
  TcpClient host{*hostDescriptor};
  TcpClient member{*memberDescriptor};
  const auto hostWelcome = authenticate(host, kCredentialA, 1u);
  const auto memberWelcome = authenticate(member, kCredentialB, 2u);
  const auto hostCapability = requestCapability(host, 3u);
  const auto memberCapability = requestCapability(member, 4u);
  UdpClient hostUdp;
  UdpClient memberUdp;
  if (!hostWelcome.has_value() || !memberWelcome.has_value() ||
      !hostCapability.has_value() || !memberCapability.has_value() ||
      !bindUdp(hostUdp, *ports, *hostWelcome, *hostCapability) ||
      !bindUdp(memberUdp, *ports, *memberWelcome, *memberCapability)) {
    return false;
  }
  const auto roomId = createRoom(host, 5u, "resume-room");
  if (!roomId.has_value() ||
      !sendLobby(member, JoinRoom{.requestId = 6u, .roomId = *roomId}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kRoomDetailMessageId, 2s).has_value() ||
      !startBattle(host, member, *roomId, 1u, 10u)) {
    return false;
  }
  host.closeNow();
  if (!attackWithoutOutboundAck(hostUdp, 999u)) {
    return false;
  }
  std::this_thread::sleep_for(100ms);

  auto resumedDescriptor = connectTcp(ports->tcp);
  if (!resumedDescriptor.has_value()) {
    return false;
  }
  TcpClient resumedClient{*resumedDescriptor};
  std::string freshCredential(43u, 'A');
  freshCredential.back() = 'Z';
  const auto resumed = resumeBattle(resumedClient, *hostWelcome,
                                    std::move(freshCredential), 20u);
  if (!resumed.has_value() ||
      resumed->welcome.sessionId != hostWelcome->sessionId ||
      resumed->welcome.sessionGeneration != hostWelcome->sessionGeneration ||
      resumed->snapshot.roomId != *roomId ||
      resumed->snapshot.battleInstanceId != 1u ||
      resumed->snapshot.playerSessionId != hostWelcome->sessionId ||
      resumed->snapshot.sessionGeneration != hostWelcome->sessionGeneration ||
      resumed->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Combat ||
      resumed->snapshot.players.size() != 2u ||
      !resumed->snapshot.monster.has_value() ||
      resumed->snapshot.monster->hitPoints != 1600u ||
      resumed->snapshot.monster->maximumHitPoints != 1600u ||
      resumed->snapshot.monster->state != 0u ||
      std::ranges::any_of(resumed->snapshot.players,
                          [](const auto &player) {
                            return player.healthKnown ||
                                   player.hitPoints != 0u ||
                                   player.maximumHitPoints != 0u ||
                                   !player.alive;
                          }) ||
      !resumed->snapshot.drops.empty() || resumed->snapshot.score != 0u ||
      resumed->snapshot.result.has_value() ||
      resumed->snapshot.remainingMillis == 0u ||
      resumed->snapshot.remainingMillis >= 30000u) {
    return false;
  }
  const auto forbiddenCapability = SessionProtocolCodec::encodeFrame(
      RequestRudpBindCapability{.requestId = 21u});
  if (!forbiddenCapability.has_value() ||
      !resumedClient.send(*forbiddenCapability) ||
      !resumedClient.waitForClose(2s)) {
    return false;
  }
  std::this_thread::sleep_for(100ms);

  auto finalDescriptor = connectTcp(ports->tcp);
  if (!finalDescriptor.has_value()) {
    return false;
  }
  TcpClient finalClient{*finalDescriptor};
  std::string retryCredential(43u, 'A');
  retryCredential.back() = 'X';
  const auto finalResume =
      resumeBattle(finalClient, *hostWelcome, std::move(retryCredential), 22u);
  if (!finalResume.has_value() ||
      finalResume->welcome.sessionId != hostWelcome->sessionId ||
      finalResume->snapshot.roomId != *roomId ||
      finalResume->snapshot.battleInstanceId != 1u) {
    return false;
  }
  const auto applied =
      SessionProtocolCodec::encodeFrame(BattleResumeSnapshotApplied{
          .snapshotId = finalResume->snapshot.snapshotId});
  if (!applied.has_value() || !finalClient.send(*applied)) {
    return false;
  }
  const auto resumedCapability = requestCapability(finalClient, 23u);
  UdpClient resumedUdp;
  const bool rebound =
      resumedCapability.has_value() &&
      bindUdp(resumedUdp, *ports, finalResume->welcome, *resumedCapability) &&
      heartbeat(resumedUdp) && heartbeat(memberUdp);
  const auto exitCode = child.stop(SIGTERM, 8s);
  return rebound && exitCode.has_value() && *exitCode == 0;
}

bool productionEntryResumesIntoResultWhenBattleEndsOffline() {
  TemporaryDirectory directory;
  if (!directory.valid() ||
      !writeText(directory.path() / "meta.credential",
                 "fixture-service-credential", S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "server.conf", configText(directory),
                 S_IRUSR | S_IWUSR)) {
    return false;
  }
  ChildProcess child;
  if (!child.start(directory.path() / "server.conf")) {
    return false;
  }
  const auto line = child.readLine(5s);
  const auto ports = line.has_value() ? parseReady(*line) : std::nullopt;
  auto hostDescriptor =
      ports.has_value() ? connectTcp(ports->tcp) : std::nullopt;
  auto memberDescriptor =
      ports.has_value() ? connectTcp(ports->tcp) : std::nullopt;
  if (!ports.has_value() || !hostDescriptor.has_value() ||
      !memberDescriptor.has_value()) {
    return false;
  }
  TcpClient host{*hostDescriptor};
  TcpClient member{*memberDescriptor};
  const auto hostWelcome = authenticate(host, kCredentialA, 1u);
  const auto memberWelcome = authenticate(member, kCredentialB, 2u);
  const auto hostCapability = requestCapability(host, 3u);
  const auto memberCapability = requestCapability(member, 4u);
  UdpClient hostUdp;
  UdpClient memberUdp;
  if (!hostWelcome.has_value() || !memberWelcome.has_value() ||
      !hostCapability.has_value() || !memberCapability.has_value() ||
      !bindUdp(hostUdp, *ports, *hostWelcome, *hostCapability) ||
      !bindUdp(memberUdp, *ports, *memberWelcome, *memberCapability)) {
    return false;
  }
  const auto roomId = createRoom(host, 5u, "offline-result-room");
  if (!roomId.has_value() ||
      !sendLobby(member, JoinRoom{.requestId = 6u, .roomId = *roomId}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kRoomDetailMessageId, 2s).has_value() ||
      !startBattle(host, member, *roomId, 1u, 10u)) {
    return false;
  }

  const auto disconnectAt = std::chrono::steady_clock::now() + 27s;
  while (std::chrono::steady_clock::now() < disconnectAt) {
    if (!heartbeat(hostUdp) || !heartbeat(memberUdp)) {
      return false;
    }
    std::this_thread::sleep_for(250ms);
  }
  host.closeNow();

  bool finalSeen = false;
  const auto resultDeadline = std::chrono::steady_clock::now() + 6s;
  while (std::chrono::steady_clock::now() < resultDeadline && !finalSeen) {
    if (!heartbeat(memberUdp)) {
      return false;
    }
    member.pump(20ms);
    finalSeen = member.take(kFinalResultMessageId, 1ms).has_value();
    std::this_thread::sleep_for(100ms);
  }
  if (!finalSeen) {
    return false;
  }

  auto resumedDescriptor = connectTcp(ports->tcp);
  if (!resumedDescriptor.has_value()) {
    return false;
  }
  TcpClient resumedClient{*resumedDescriptor};
  std::string freshCredential(43u, 'A');
  freshCredential.back() = 'Y';
  const auto resumed = resumeBattle(resumedClient, *hostWelcome,
                                    std::move(freshCredential), 20u);
  if (!resumed.has_value() ||
      resumed->welcome.sessionId != hostWelcome->sessionId ||
      resumed->snapshot.roomId != *roomId ||
      resumed->snapshot.battleInstanceId != 1u ||
      resumed->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Result ||
      resumed->snapshot.remainingMillis != 0u ||
      resumed->snapshot.players.size() != 2u ||
      !resumed->snapshot.monster.has_value() ||
      resumed->snapshot.monster->state != 3u ||
      !resumed->snapshot.drops.empty() || resumed->snapshot.score != 0u ||
      !resumed->snapshot.result.has_value() ||
      resumed->snapshot.result->outcome !=
          lol::transport::tcp::FinalResultOutcome::CombatTimeout ||
      resumed->snapshot.result->entries.size() != 2u) {
    return false;
  }
  const auto applied = SessionProtocolCodec::encodeFrame(
      BattleResumeSnapshotApplied{.snapshotId = resumed->snapshot.snapshotId});
  if (!applied.has_value() || !resumedClient.send(*applied)) {
    return false;
  }

  member.closeNow();
  std::this_thread::sleep_for(100ms);
  auto postFinalDescriptor = connectTcp(ports->tcp);
  if (!postFinalDescriptor.has_value()) {
    return false;
  }
  TcpClient postFinalClient{*postFinalDescriptor};
  std::string postFinalCredential(43u, 'B');
  postFinalCredential.back() = 'Z';
  const auto postFinal = resumeBattle(postFinalClient, *memberWelcome,
                                      std::move(postFinalCredential), 21u);
  if (!postFinal.has_value() ||
      postFinal->welcome.sessionId != memberWelcome->sessionId ||
      postFinal->snapshot.roomId != *roomId ||
      postFinal->snapshot.battleInstanceId != 1u ||
      postFinal->snapshot.playerSessionId != memberWelcome->sessionId ||
      postFinal->snapshot.sessionGeneration !=
          memberWelcome->sessionGeneration ||
      postFinal->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Result ||
      postFinal->snapshot.remainingMillis != 0u ||
      !postFinal->snapshot.result.has_value() ||
      postFinal->snapshot.result->outcome !=
          lol::transport::tcp::FinalResultOutcome::CombatTimeout ||
      postFinal->snapshot.result->entries.size() != 2u) {
    return false;
  }
  const auto postFinalApplied =
      SessionProtocolCodec::encodeFrame(BattleResumeSnapshotApplied{
          .snapshotId = postFinal->snapshot.snapshotId});
  if (!postFinalApplied.has_value() ||
      !postFinalClient.send(*postFinalApplied)) {
    return false;
  }

  postFinalClient.closeNow();
  std::this_thread::sleep_for(31s);
  auto expiredDescriptor = connectTcp(ports->tcp);
  if (!expiredDescriptor.has_value()) {
    return false;
  }
  TcpClient expiredClient{*expiredDescriptor};
  std::string expiredCredential(43u, 'B');
  expiredCredential.back() = 'X';
  const auto expiredRequest =
      SessionProtocolCodec::encodeFrame(ResumeBattleSession{
          .requestId = 22u,
          .previousSessionId = memberWelcome->sessionId,
          .previousSessionGeneration = memberWelcome->sessionGeneration,
          .credential = std::move(expiredCredential),
      });
  if (!expiredRequest.has_value() || !expiredClient.send(*expiredRequest)) {
    return false;
  }
  const auto rejectedFrame =
      expiredClient.take(kAuthenticationRejectedMessageId, 3s);
  if (!rejectedFrame.has_value()) {
    return false;
  }
  const auto rejected = SessionProtocolCodec::decodeFrame(*rejectedFrame);
  const auto *rejection =
      rejected.message.has_value()
          ? std::get_if<AuthenticationRejected>(&*rejected.message)
          : nullptr;
  if (rejection == nullptr || rejection->requestId != 22u ||
      rejection->reason != AuthenticationRejectedReason::ResumeUnavailable ||
      resumedClient.take(kFinalResultMessageId, 100ms).has_value()) {
    return false;
  }
  const auto exitCode = child.stop(SIGTERM, 8s);
  return exitCode.has_value() && *exitCode == 0;
}

bool productionEntryRetiresTerminalBattleAfterReconnectGrace() {
  TemporaryDirectory directory;
  const auto continuityRoot = directory.path() / "battle-continuity";
  std::error_code directoryError;
  if (!directory.valid() ||
      !std::filesystem::create_directory(continuityRoot, directoryError) ||
      directoryError || ::chmod(continuityRoot.c_str(), S_IRWXU) != 0 ||
      !writeText(directory.path() / "battle-continuity.key",
                 std::string(32u, 'R'), S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "meta.credential",
                 "fixture-service-credential", S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "server.conf",
                 configText(directory, 0u, 0u, std::nullopt, true),
                 S_IRUSR | S_IWUSR)) {
    return false;
  }

  const auto configPath = directory.path() / "server.conf";
  ChildProcess first;
  if (!first.start(configPath)) {
    return false;
  }
  const auto ports = readRecoveryReady(first, 1u);
  auto hostDescriptor =
      ports.has_value() ? connectTcp(ports->tcp) : std::nullopt;
  auto memberDescriptor =
      ports.has_value() ? connectTcp(ports->tcp) : std::nullopt;
  if (!ports.has_value() || !hostDescriptor.has_value() ||
      !memberDescriptor.has_value()) {
    return false;
  }
  TcpClient host{*hostDescriptor};
  TcpClient member{*memberDescriptor};
  const auto hostWelcome = authenticate(host, kCredentialA, 1u);
  const auto memberWelcome = authenticate(member, kCredentialB, 2u);
  const auto hostCapability = requestCapability(host, 3u);
  const auto memberCapability = requestCapability(member, 4u);
  UdpClient hostUdp;
  UdpClient memberUdp;
  if (!hostWelcome.has_value() || !memberWelcome.has_value() ||
      !hostCapability.has_value() || !memberCapability.has_value() ||
      !bindUdp(hostUdp, *ports, *hostWelcome, *hostCapability) ||
      !bindUdp(memberUdp, *ports, *memberWelcome, *memberCapability)) {
    return false;
  }
  const auto roomId = createRoom(host, 5u, "terminal-retirement-room");
  if (!roomId.has_value() ||
      !sendLobby(member, JoinRoom{.requestId = 6u, .roomId = *roomId}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kRoomDetailMessageId, 2s).has_value() ||
      !startBattle(host, member, *roomId, 1u, 10u)) {
    return false;
  }

  bool finalSeen = false;
  const auto resultDeadline = std::chrono::steady_clock::now() + 35s;
  while (std::chrono::steady_clock::now() < resultDeadline && !finalSeen) {
    if (!heartbeat(hostUdp) || !heartbeat(memberUdp)) {
      return false;
    }
    host.pump(20ms);
    member.pump(20ms);
    finalSeen = member.take(kFinalResultMessageId, 1ms).has_value();
    std::this_thread::sleep_for(100ms);
  }
  if (!finalSeen) {
    return false;
  }

  host.closeNow();
  member.closeNow();
  std::this_thread::sleep_for(100ms);
  std::this_thread::sleep_for(31s);
  const auto firstExit = first.stop(SIGTERM, 8s);
  if (!firstExit.has_value() || *firstExit != 0) {
    return false;
  }

  auto hasRetiredMarker = [&] {
    std::error_code scanError;
    for (const auto &entry :
         std::filesystem::directory_iterator(continuityRoot, scanError)) {
      if (entry.path().filename().string().ends_with(
              "-0000000000000001.retired")) {
        return true;
      }
    }
    return false;
  };
  if (!hasRetiredMarker()) {
    return false;
  }

  ChildProcess second;
  const auto secondPorts = second.start(configPath)
                               ? readRecoveryReady(second, 2u)
                               : std::nullopt;
  if (!secondPorts.has_value()) {
    return false;
  }
  auto resumedDescriptor = connectTcp(secondPorts->tcp);
  if (!resumedDescriptor.has_value()) {
    return false;
  }
  TcpClient resumed{*resumedDescriptor};
  std::string credential(43u, 'B');
  credential.back() = 'R';
  const auto request = SessionProtocolCodec::encodeFrame(ResumeBattleSession{
      .requestId = 7u,
      .previousSessionId = memberWelcome->sessionId,
      .previousSessionGeneration = memberWelcome->sessionGeneration,
      .credential = std::move(credential),
  });
  if (!request.has_value() || !resumed.send(*request)) {
    return false;
  }
  const auto rejectedFrame =
      resumed.take(kAuthenticationRejectedMessageId, 3s);
  if (!rejectedFrame.has_value()) {
    return false;
  }
  const auto rejected = SessionProtocolCodec::decodeFrame(*rejectedFrame);
  const auto *rejection =
      rejected.message.has_value()
          ? std::get_if<AuthenticationRejected>(&*rejected.message)
          : nullptr;
  const auto secondExit = second.stop(SIGTERM, 8s);
  return rejection != nullptr && rejection->requestId == 7u &&
         rejection->reason == AuthenticationRejectedReason::ResumeUnavailable &&
         secondExit.has_value() && *secondExit == 0;
}

bool productionEntryRestoresBattleAfterProcessKill() {
  TemporaryDirectory directory;
  const auto continuityRoot = directory.path() / "battle-continuity";
  std::error_code directoryError;
  if (!directory.valid() ||
      !std::filesystem::create_directory(continuityRoot, directoryError) ||
      directoryError || ::chmod(continuityRoot.c_str(), S_IRWXU) != 0 ||
      !writeText(directory.path() / "battle-continuity.key",
                 std::string(32u, 'K'), S_IRUSR | S_IWUSR) ||
      !writeText(directory.path() / "meta.credential",
                 "fixture-service-credential", S_IRUSR | S_IWUSR)) {
    return false;
  }

  auto makeConfig = [&](std::uint16_t tcpPort, std::uint16_t udpPort) {
    auto config = configText(directory, tcpPort, udpPort, std::nullopt, true);
    constexpr std::string_view available =
        "meta_settlements_url=https://meta.test/internal/v1/settlements";
    constexpr std::string_view unavailable =
        "meta_settlements_url=https://meta.test/internal/v1/offline";
    const auto offset = config.find(available);
    if (offset == std::string::npos) {
      return std::string{};
    }
    config.replace(offset, available.size(), unavailable);
    return config;
  };

  const auto configPath = directory.path() / "server.conf";
  const auto initialConfig = makeConfig(0u, 0u);
  if (initialConfig.empty() ||
      !writeText(configPath, initialConfig, S_IRUSR | S_IWUSR)) {
    return false;
  }

  ChildProcess first;
  if (!first.start(configPath)) {
    return false;
  }
  const auto ports = readRecoveryReady(first, 1u);
  auto hostDescriptor =
      ports.has_value() ? connectTcp(ports->tcp) : std::nullopt;
  auto memberDescriptor =
      ports.has_value() ? connectTcp(ports->tcp) : std::nullopt;
  if (!ports.has_value() || !hostDescriptor.has_value() ||
      !memberDescriptor.has_value()) {
    return false;
  }

  TcpClient host{*hostDescriptor};
  TcpClient member{*memberDescriptor};
  const auto hostWelcome = authenticate(host, kCredentialA, 1u);
  const auto memberWelcome = authenticate(member, kCredentialB, 2u);
  const auto hostCapability = requestCapability(host, 3u);
  const auto memberCapability = requestCapability(member, 4u);
  UdpClient hostUdp;
  UdpClient memberUdp;
  if (!hostWelcome.has_value() || !memberWelcome.has_value() ||
      !hostCapability.has_value() || !memberCapability.has_value() ||
      !bindUdp(hostUdp, *ports, *hostWelcome, *hostCapability) ||
      !bindUdp(memberUdp, *ports, *memberWelcome, *memberCapability)) {
    return false;
  }

  const auto roomId = createRoom(host, 5u, "crash-continuity-room");
  if (!roomId.has_value() || (*roomId >> 32u) != 1u ||
      !sendLobby(member, JoinRoom{.requestId = 6u, .roomId = *roomId}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kRoomDetailMessageId, 2s).has_value() ||
      !startBattle(host, member, *roomId, 1u, 10u) ||
      !sendMove(hostUdp, 1u, 32767, 0)) {
    return false;
  }

  const auto moved =
      waitForMovementSnapshot(hostUdp, [&](const RudpStateSnapshot &snapshot) {
        const auto player =
            std::ranges::find_if(snapshot.players, [&](const auto &candidate) {
              return candidate.sessionId == hostWelcome->sessionId;
            });
        return player != snapshot.players.end() &&
               player->posXMillimeter > 2500;
      });
  if (!moved.has_value() || !sendMove(hostUdp, 2u, 0, 0)) {
    return false;
  }
  const auto stopped =
      waitForMovementSnapshot(hostUdp, [&](const RudpStateSnapshot &snapshot) {
        return snapshot.serverTick > moved->serverTick;
      });
  if (!stopped.has_value() || !sendAttack(hostUdp, 1001u)) {
    return false;
  }
  const auto applied = waitForAttackApplied(hostUdp, 1500u);
  const auto stoppedHost =
      std::ranges::find_if(stopped->players, [&](const auto &candidate) {
        return candidate.sessionId == hostWelcome->sessionId;
      });
  if (!applied.has_value() || stoppedHost == stopped->players.end()) {
    return false;
  }
  const auto durablePositionX = stoppedHost->posXMillimeter;
  const auto durablePositionY = stoppedHost->posYMillimeter;
  const auto durableServerTick = stopped->serverTick;

  const auto fixedConfig = makeConfig(ports->tcp, ports->udp);
  if (fixedConfig.empty() ||
      !writeText(configPath, fixedConfig, S_IRUSR | S_IWUSR)) {
    return false;
  }
  const auto killedAt = std::chrono::steady_clock::now();
  const auto killed = first.stop(SIGKILL, 5s);
  if (!killed.has_value() || *killed != -SIGKILL) {
    return false;
  }

  ChildProcess restarted;
  if (!restarted.start(configPath)) {
    return false;
  }
  const auto recoveredPorts = readRecoveryReady(restarted, 2u);
  const auto readyAt = std::chrono::steady_clock::now();
  if (!recoveredPorts.has_value() || recoveredPorts->tcp != ports->tcp ||
      recoveredPorts->udp != ports->udp ||
      !attackWithoutOutboundAck(hostUdp, 1002u)) {
    return false;
  }
  std::this_thread::sleep_for(150ms);

  auto resumedHostDescriptor = connectTcp(recoveredPorts->tcp);
  auto resumedMemberDescriptor = connectTcp(recoveredPorts->tcp);
  if (!resumedHostDescriptor.has_value() ||
      !resumedMemberDescriptor.has_value()) {
    return false;
  }
  TcpClient resumedHost{*resumedHostDescriptor};
  TcpClient resumedMember{*resumedMemberDescriptor};
  std::string freshHostCredential(43u, 'A');
  freshHostCredential.back() = 'R';
  std::string freshMemberCredential(43u, 'B');
  freshMemberCredential.back() = 'R';
  const auto hostResume = resumeBattle(resumedHost, *hostWelcome,
                                       std::move(freshHostCredential), 30u);
  const auto memberResume = resumeBattle(resumedMember, *memberWelcome,
                                         std::move(freshMemberCredential), 31u);
  const auto snapshotAt = std::chrono::steady_clock::now();
  std::cerr << "continuity resume host=" << hostResume.has_value()
            << " member=" << memberResume.has_value();
  if (hostResume.has_value()) {
    std::cerr << " old_gen=" << hostWelcome->sessionGeneration
              << " new_gen=" << hostResume->welcome.sessionGeneration
              << " room=" << hostResume->snapshot.roomId
              << " battle=" << hostResume->snapshot.battleInstanceId
              << " tick=" << hostResume->snapshot.serverTick << " hp="
              << (hostResume->snapshot.monster.has_value()
                      ? hostResume->snapshot.monster->hitPoints
                      : 0u);
  }
  if (memberResume.has_value()) {
    std::cerr << " member_old_gen=" << memberWelcome->sessionGeneration
              << " member_new_gen=" << memberResume->welcome.sessionGeneration
              << " member_room=" << memberResume->snapshot.roomId
              << " member_battle=" << memberResume->snapshot.battleInstanceId
              << " member_tick=" << memberResume->snapshot.serverTick
              << " member_hp="
              << (memberResume->snapshot.monster.has_value()
                      ? memberResume->snapshot.monster->hitPoints
                      : 0u);
  }
  std::cerr << '\n';
  if (!hostResume.has_value() || !memberResume.has_value() ||
      hostResume->welcome.sessionId != hostWelcome->sessionId ||
      memberResume->welcome.sessionId != memberWelcome->sessionId ||
      hostResume->welcome.sessionGeneration <= hostWelcome->sessionGeneration ||
      memberResume->welcome.sessionGeneration <=
          memberWelcome->sessionGeneration ||
      hostResume->snapshot.roomId != *roomId ||
      memberResume->snapshot.roomId != *roomId ||
      hostResume->snapshot.battleInstanceId != 1u ||
      memberResume->snapshot.battleInstanceId != 1u ||
      hostResume->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Combat ||
      memberResume->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Combat ||
      !hostResume->snapshot.monster.has_value() ||
      !memberResume->snapshot.monster.has_value() ||
      hostResume->snapshot.monster->hitPoints != 1500u ||
      memberResume->snapshot.monster->hitPoints != 1500u ||
      hostResume->snapshot.serverTick < durableServerTick ||
      memberResume->snapshot.serverTick < durableServerTick) {
    return false;
  }
  const auto recoveredHost = std::ranges::find_if(
      hostResume->snapshot.players, [&](const auto &candidate) {
        return candidate.sessionId == hostWelcome->sessionId;
      });
  if (recoveredHost != hostResume->snapshot.players.end()) {
    std::cerr << "continuity position durable=" << durablePositionX << ','
              << durablePositionY
              << " recovered=" << recoveredHost->positionXMillimeters << ','
              << recoveredHost->positionYMillimeters
              << " durable_tick=" << durableServerTick << '\n';
  }
  if (recoveredHost == hostResume->snapshot.players.end() ||
      recoveredHost->positionXMillimeters != durablePositionX ||
      recoveredHost->positionYMillimeters != durablePositionY) {
    return false;
  }

  const auto hostAck =
      SessionProtocolCodec::encodeFrame(BattleResumeSnapshotApplied{
          .snapshotId = hostResume->snapshot.snapshotId});
  const auto memberAck =
      SessionProtocolCodec::encodeFrame(BattleResumeSnapshotApplied{
          .snapshotId = memberResume->snapshot.snapshotId});
  if (!hostAck.has_value() || !memberAck.has_value() ||
      !resumedHost.send(*hostAck) || !resumedMember.send(*memberAck)) {
    return false;
  }
  const auto resumedHostCapability = requestCapability(resumedHost, 32u);
  const auto resumedMemberCapability = requestCapability(resumedMember, 33u);
  UdpClient resumedHostUdp;
  UdpClient resumedMemberUdp;
  const bool hostBound = resumedHostCapability.has_value() &&
                         bindUdp(resumedHostUdp, *recoveredPorts,
                                 hostResume->welcome, *resumedHostCapability);
  const bool memberBound =
      resumedMemberCapability.has_value() &&
      bindUdp(resumedMemberUdp, *recoveredPorts, memberResume->welcome,
              *resumedMemberCapability);
  std::cerr << "continuity rebound host_cap="
            << resumedHostCapability.has_value()
            << " member_cap=" << resumedMemberCapability.has_value()
            << " host_bound=" << hostBound << " member_bound=" << memberBound
            << '\n';
  if (!hostBound || !memberBound || !sendMove(resumedHostUdp, 3u, -32767, 0)) {
    return false;
  }
  const auto postRecoveryMove = waitForMovementSnapshot(
      resumedHostUdp, [&](const RudpStateSnapshot &snapshot) {
        const auto player =
            std::ranges::find_if(snapshot.players, [&](const auto &candidate) {
              return candidate.sessionId == hostWelcome->sessionId;
            });
        return player != snapshot.players.end() &&
               player->posXMillimeter < durablePositionX;
      });
  std::cerr << "continuity post_recovery_move=" << postRecoveryMove.has_value()
            << '\n';
  if (!postRecoveryMove.has_value() || !sendMove(resumedHostUdp, 4u, 0, 0)) {
    return false;
  }

  for (std::uint64_t index = 0u; index < 15u; ++index) {
    const auto expectedHitPoints =
        static_cast<std::uint32_t>(1400u - (index * 100u));
    if (!sendAttack(resumedMemberUdp, 2000u + index) ||
        !waitForAttackApplied(resumedMemberUdp, expectedHitPoints, 3s)
             .has_value()) {
      std::cerr << "continuity attack send failed index=" << index << '\n';
      return false;
    }
    if (index + 1u < 15u) {
      if (!heartbeat(resumedHostUdp)) {
        return false;
      }
      std::this_thread::sleep_for(800ms);
    }
  }
  std::cerr << "continuity attacks sent\n";
  const auto drops = waitForDrops(resumedHostUdp, 5s);
  std::cerr << "continuity drops=" << drops.has_value() << '\n';
  if (!drops.has_value()) {
    return false;
  }

  auto moveToDrop = [&](UdpClient &client, std::uint64_t sessionId,
                        std::uint32_t &actionSequence,
                        const auto &drop) -> bool {
    const auto current =
        waitForMovementSnapshot(client, [&](const RudpStateSnapshot &snapshot) {
          return std::ranges::any_of(snapshot.players, [&](const auto &player) {
            return player.sessionId == sessionId;
          });
        });
    if (!current.has_value()) {
      std::cerr << "continuity move_to_drop missing snapshot session="
                << sessionId << '\n';
      return false;
    }
    const auto player =
        std::ranges::find_if(current->players, [&](const auto &candidate) {
          return candidate.sessionId == sessionId;
        });
    if (player == current->players.end()) {
      return false;
    }
    const auto deltaX = drop.posXMillimeter - player->posXMillimeter;
    const auto deltaY = drop.posYMillimeter - player->posYMillimeter;
    const auto near = [](std::int64_t x, std::int64_t y) {
      return x * x + y * y <= 1'000'000LL;
    };
    if (!near(deltaX, deltaY) &&
        !sendMove(client, actionSequence++, static_cast<std::int16_t>(deltaX),
                  static_cast<std::int16_t>(deltaY))) {
      std::cerr << "continuity move_to_drop send failed session=" << sessionId
                << '\n';
      return false;
    }
    const auto reached =
        near(deltaX, deltaY)
            ? current
            : waitForMovementSnapshot(
                  client,
                  [&](const RudpStateSnapshot &snapshot) {
                    const auto candidate = std::ranges::find_if(
                        snapshot.players, [&](const auto &value) {
                          return value.sessionId == sessionId;
                        });
                    if (candidate == snapshot.players.end()) {
                      return false;
                    }
                    return near(static_cast<std::int64_t>(drop.posXMillimeter) -
                                    candidate->posXMillimeter,
                                static_cast<std::int64_t>(drop.posYMillimeter) -
                                    candidate->posYMillimeter);
                  },
                  8s);
    const bool stoppedAtDrop =
        reached.has_value() && sendMove(client, actionSequence++, 0, 0);
    std::cerr << "continuity move_to_drop session=" << sessionId
              << " reached=" << reached.has_value()
              << " stopped=" << stoppedAtDrop << '\n';
    return stoppedAtDrop;
  };

  std::uint32_t hostActionSequence = 5u;
  std::uint32_t memberActionSequence = 1u;
  if (!moveToDrop(resumedHostUdp, hostWelcome->sessionId, hostActionSequence,
                  drops->drops[0]) ||
      !moveToDrop(resumedMemberUdp, memberWelcome->sessionId,
                  memberActionSequence, drops->drops[1]) ||
      !sendClaimLoot(resumedHostUdp, 3001u, drops->drops[0].dropId) ||
      !waitForClaimAccepted(resumedHostUdp, drops->drops[0].dropId) ||
      !sendClaimLoot(resumedMemberUdp, 3002u, drops->drops[1].dropId) ||
      !waitForClaimAccepted(resumedMemberUdp, drops->drops[1].dropId) ||
      !waitForCycleCompletion(resumedHost, resumedMember, resumedHostUdp,
                              resumedMemberUdp)) {
    return false;
  }

  const auto terminalKilled = restarted.stop(SIGKILL, 5s);
  if (!terminalKilled.has_value() || *terminalKilled != -SIGKILL) {
    return false;
  }
  const auto settlementAvailableConfig =
      configText(directory, ports->tcp, ports->udp, std::nullopt, true);
  if (!writeText(configPath, settlementAvailableConfig, S_IRUSR | S_IWUSR)) {
    return false;
  }
  ChildProcess terminalRecovery;
  if (!terminalRecovery.start(configPath)) {
    return false;
  }
  const auto terminalPorts = readRecoveryReady(terminalRecovery, 3u);
  std::this_thread::sleep_for(300ms);
  auto terminalHostDescriptor =
      terminalPorts.has_value() ? connectTcp(terminalPorts->tcp) : std::nullopt;
  auto terminalMemberDescriptor =
      terminalPorts.has_value() ? connectTcp(terminalPorts->tcp) : std::nullopt;
  if (!terminalPorts.has_value() || !terminalHostDescriptor.has_value() ||
      !terminalMemberDescriptor.has_value()) {
    return false;
  }

  TcpClient terminalHost{*terminalHostDescriptor};
  TcpClient terminalMember{*terminalMemberDescriptor};
  std::string terminalHostCredential(43u, 'A');
  terminalHostCredential.back() = 'S';
  std::string terminalMemberCredential(43u, 'B');
  terminalMemberCredential.back() = 'S';
  const auto terminalHostResume = resumeBattle(
      terminalHost, *hostWelcome, std::move(terminalHostCredential), 40u);
  const auto terminalMemberResume = resumeBattle(
      terminalMember, *memberWelcome, std::move(terminalMemberCredential), 41u);
  std::cerr << "continuity terminal resume host="
            << terminalHostResume.has_value()
            << " member=" << terminalMemberResume.has_value();
  if (terminalHostResume.has_value()) {
    std::cerr << " host_battle="
              << terminalHostResume->snapshot.battleInstanceId << " host_phase="
              << static_cast<int>(terminalHostResume->snapshot.phase)
              << " host_generation="
              << terminalHostResume->welcome.sessionGeneration;
  }
  if (terminalMemberResume.has_value()) {
    std::cerr << " member_battle="
              << terminalMemberResume->snapshot.battleInstanceId
              << " member_phase="
              << static_cast<int>(terminalMemberResume->snapshot.phase)
              << " member_generation="
              << terminalMemberResume->welcome.sessionGeneration;
  }
  std::cerr << '\n';
  if (!terminalHostResume.has_value() || !terminalMemberResume.has_value() ||
      terminalHostResume->welcome.sessionGeneration <=
          hostResume->welcome.sessionGeneration ||
      terminalMemberResume->welcome.sessionGeneration <=
          memberResume->welcome.sessionGeneration ||
      terminalHostResume->snapshot.roomId != *roomId ||
      terminalMemberResume->snapshot.roomId != *roomId ||
      terminalHostResume->snapshot.battleInstanceId != 1u ||
      terminalMemberResume->snapshot.battleInstanceId != 1u ||
      terminalHostResume->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Result ||
      terminalMemberResume->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Result ||
      !terminalHostResume->snapshot.result.has_value() ||
      !terminalMemberResume->snapshot.result.has_value()) {
    return false;
  }
  const auto terminalHostAck =
      SessionProtocolCodec::encodeFrame(BattleResumeSnapshotApplied{
          .snapshotId = terminalHostResume->snapshot.snapshotId});
  const auto terminalMemberAck =
      SessionProtocolCodec::encodeFrame(BattleResumeSnapshotApplied{
          .snapshotId = terminalMemberResume->snapshot.snapshotId});
  if (!terminalHostAck.has_value() || !terminalMemberAck.has_value() ||
      !terminalHost.send(*terminalHostAck) ||
      !terminalMember.send(*terminalMemberAck)) {
    return false;
  }
  const auto terminalHostCapability = requestCapability(terminalHost, 42u);
  const auto terminalMemberCapability = requestCapability(terminalMember, 43u);
  UdpClient terminalHostUdp;
  UdpClient terminalMemberUdp;
  const bool terminalHostBound =
      terminalHostCapability.has_value() &&
      bindUdp(terminalHostUdp, *terminalPorts, terminalHostResume->welcome,
              *terminalHostCapability);
  const bool terminalMemberBound =
      terminalMemberCapability.has_value() &&
      bindUdp(terminalMemberUdp, *terminalPorts, terminalMemberResume->welcome,
              *terminalMemberCapability);

  const bool successorStarted =
      terminalHostBound && terminalMemberBound &&
      startBattle(terminalHost, terminalMember, *roomId, 2u, 50u);
  const bool successorMoveSent =
      successorStarted && sendMove(terminalHostUdp, 1u, 32767, 0, 2u);
  std::cerr << "continuity successor setup host_cap="
            << terminalHostCapability.has_value()
            << " member_cap=" << terminalMemberCapability.has_value()
            << " host_bound=" << terminalHostBound
            << " member_bound=" << terminalMemberBound
            << " started=" << successorStarted
            << " move_sent=" << successorMoveSent << '\n';
  if (!successorMoveSent) {
    return false;
  }
  const auto successorMoved = waitForMovementSnapshot(
      terminalHostUdp, [&](const RudpStateSnapshot &snapshot) {
        const auto player =
            std::ranges::find_if(snapshot.players, [&](const auto &candidate) {
              return candidate.sessionId == hostWelcome->sessionId;
            });
        return snapshot.battleInstanceId == 2u &&
               player != snapshot.players.end() &&
               player->posXMillimeter > 2500;
      });
  if (!successorMoved.has_value() || !sendMove(terminalHostUdp, 2u, 0, 0, 2u)) {
    return false;
  }
  const auto successorStopped = waitForMovementSnapshot(
      terminalHostUdp, [&](const RudpStateSnapshot &snapshot) {
        return snapshot.battleInstanceId == 2u &&
               snapshot.serverTick > successorMoved->serverTick;
      });
  if (!successorStopped.has_value() ||
      !sendAttack(terminalHostUdp, 4001u, 2u)) {
    return false;
  }
  const auto successorAttack = waitForAttackApplied(terminalHostUdp, 1500u);
  const auto successorHost =
      std::ranges::find_if(successorStopped->players, [&](const auto &player) {
        return player.sessionId == hostWelcome->sessionId;
      });
  if (!successorAttack.has_value() || successorAttack->battleInstanceId != 2u ||
      successorHost == successorStopped->players.end()) {
    return false;
  }
  const auto successorPositionX = successorHost->posXMillimeter;
  const auto successorPositionY = successorHost->posYMillimeter;
  const auto successorServerTick = successorStopped->serverTick;

  const auto successorKilled = terminalRecovery.stop(SIGKILL, 5s);
  if (!successorKilled.has_value() || *successorKilled != -SIGKILL) {
    return false;
  }
  ChildProcess successorRecovery;
  if (!successorRecovery.start(configPath)) {
    return false;
  }
  const auto successorPorts = readRecoveryReady(successorRecovery, 4u);
  auto successorHostDescriptor = successorPorts.has_value()
                                     ? connectTcp(successorPorts->tcp)
                                     : std::nullopt;
  auto successorMemberDescriptor = successorPorts.has_value()
                                       ? connectTcp(successorPorts->tcp)
                                       : std::nullopt;
  if (!successorPorts.has_value() || !successorHostDescriptor.has_value() ||
      !successorMemberDescriptor.has_value()) {
    return false;
  }
  TcpClient successorHostClient{*successorHostDescriptor};
  TcpClient successorMemberClient{*successorMemberDescriptor};
  std::string successorHostCredential(43u, 'A');
  successorHostCredential.back() = 'T';
  std::string successorMemberCredential(43u, 'B');
  successorMemberCredential.back() = 'T';
  const auto successorHostResume =
      resumeBattle(successorHostClient, terminalHostResume->welcome,
                   std::move(successorHostCredential), 60u);
  const auto successorMemberResume =
      resumeBattle(successorMemberClient, terminalMemberResume->welcome,
                   std::move(successorMemberCredential), 61u);
  if (!successorHostResume.has_value() || !successorMemberResume.has_value() ||
      successorHostResume->snapshot.roomId != *roomId ||
      successorMemberResume->snapshot.roomId != *roomId ||
      successorHostResume->snapshot.battleInstanceId != 2u ||
      successorMemberResume->snapshot.battleInstanceId != 2u ||
      successorHostResume->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Combat ||
      successorMemberResume->snapshot.phase !=
          lol::transport::tcp::BattleResumePhase::Combat ||
      !successorHostResume->snapshot.monster.has_value() ||
      !successorMemberResume->snapshot.monster.has_value() ||
      successorHostResume->snapshot.monster->hitPoints != 1500u ||
      successorMemberResume->snapshot.monster->hitPoints != 1500u ||
      successorHostResume->snapshot.serverTick < successorServerTick ||
      successorMemberResume->snapshot.serverTick < successorServerTick) {
    return false;
  }
  const auto recoveredSuccessorHost = std::ranges::find_if(
      successorHostResume->snapshot.players, [&](const auto &player) {
        return player.sessionId == hostWelcome->sessionId;
      });
  if (recoveredSuccessorHost == successorHostResume->snapshot.players.end() ||
      recoveredSuccessorHost->positionXMillimeters != successorPositionX ||
      recoveredSuccessorHost->positionYMillimeters != successorPositionY) {
    return false;
  }
  const auto successorHostAck =
      SessionProtocolCodec::encodeFrame(BattleResumeSnapshotApplied{
          .snapshotId = successorHostResume->snapshot.snapshotId});
  const auto successorMemberAck =
      SessionProtocolCodec::encodeFrame(BattleResumeSnapshotApplied{
          .snapshotId = successorMemberResume->snapshot.snapshotId});
  if (!successorHostAck.has_value() || !successorMemberAck.has_value() ||
      !successorHostClient.send(*successorHostAck) ||
      !successorMemberClient.send(*successorMemberAck)) {
    return false;
  }
  const auto successorHostCapability =
      requestCapability(successorHostClient, 62u);
  const auto successorMemberCapability =
      requestCapability(successorMemberClient, 63u);
  UdpClient successorHostUdp;
  UdpClient successorMemberUdp;
  if (!successorHostCapability.has_value() ||
      !successorMemberCapability.has_value() ||
      !bindUdp(successorHostUdp, *successorPorts, successorHostResume->welcome,
               *successorHostCapability) ||
      !bindUdp(successorMemberUdp, *successorPorts,
               successorMemberResume->welcome, *successorMemberCapability) ||
      !heartbeat(successorHostUdp) || !heartbeat(successorMemberUdp)) {
    return false;
  }
  const auto successorExit = successorRecovery.stop(SIGTERM, 8s);
  if (!successorExit.has_value() || *successorExit != 0) {
    return false;
  }

  lol::settlement_storage::SegmentJournal settlementJournal{directory.path() /
                                                            "outbox.journal"};
  const auto settlementRecovery = settlementJournal.recoverAndRepair();
  if (!settlementRecovery.has_value() ||
      std::ranges::any_of(settlementRecovery->batches,
                          [](const auto &batch) { return !batch.retired; })) {
    return false;
  }

  auto continuity = lol::battle_continuity_storage::ContinuityStorage::open(
      continuityRoot, directory.path() / "battle-continuity.key");
  if (!continuity.ok()) {
    return false;
  }
  const auto continuityScan = continuity.storage->scan();
  if (!continuityScan.ok() || !continuityScan.quarantined.empty() ||
      continuityScan.healthy.size() != 1u ||
      continuityScan.healthy.front().identity.roomId.value() != *roomId ||
      continuityScan.healthy.front().identity.battleInstanceId.value() != 2u) {
    continuity.storage->stop();
    return false;
  }
  const lol::battle_continuity::BattleIdentity identity{
      .originRecoveryEpoch = 1u,
      .roomId = lol::shared::RoomId{*roomId},
      .battleInstanceId = lol::shared::BattleInstanceId{1u},
  };
  const auto journal = readBytes(continuity.storage->journalPath(identity));
  continuity.storage->stop();
  if (!journal.has_value()) {
    return false;
  }
  const auto replayed =
      lol::battle_continuity::BattleReplayer::replayJournal(*journal);
  const auto restored =
      lol::battle_continuity::BattleReplayer::restoreJournal(*journal);
  if (!replayed.ok() || !restored.ok() ||
      !replayed.finalStateHash.has_value() ||
      replayed.finalStateHash != restored.finalStateHash ||
      !replayed.battle.has_value() ||
      replayed.battle->resultProjection().state !=
          lol::battle::BattleResultState::Committed) {
    return false;
  }

  const auto killToReady =
      std::chrono::duration_cast<std::chrono::milliseconds>(readyAt - killedAt);
  const auto killToSnapshot =
      std::chrono::duration_cast<std::chrono::milliseconds>(snapshotAt -
                                                            killedAt);
  std::cout << "CRASH_RECOVERY_EVIDENCE room=" << *roomId
            << " pre_tick=" << durableServerTick
            << " recovered_tick=" << hostResume->snapshot.serverTick
            << " observed_command_rpo=0"
            << " kill_to_ready_ms=" << killToReady.count()
            << " kill_to_snapshot_ms=" << killToSnapshot.count()
            << " unretired_settlement_batches=0"
            << " successor_battle=2"
            << " successor_recovered_tick="
            << successorHostResume->snapshot.serverTick
            << " final_hash=" << hashHex(*replayed.finalStateHash) << '\n';
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view{argv[1]} == "movement-ack") {
    return productionEntryAcceptsReliableAckPiggybackedOnMove() ? EXIT_SUCCESS
                                                                : 1;
  }
  if (argc == 2 && std::string_view{argv[1]} == "continuity") {
    return productionEntryRestoresBattleAfterProcessKill() ? EXIT_SUCCESS : 1;
  }
  if (argc == 2 && std::string_view{argv[1]} == "terminal-retirement") {
    return productionEntryRetiresTerminalBattleAfterReconnectGrace()
               ? EXIT_SUCCESS
               : 1;
  }
  if (argc == 2 && std::string_view{argv[1]} == "reconnect") {
    return productionEntryResumesSameSessionRoomAndBattleWithinGrace() &&
                   productionEntryResumesIntoResultWhenBattleEndsOffline()
               ? EXIT_SUCCESS
               : 1;
  }
  if (argc != 1) {
    return 2;
  }
  if (!invalidConfigFailsBeforeListener()) {
    return 1;
  }
  if (!productionEntryServesPrivateMetrics()) {
    return 2;
  }
  if (!productionEntryClosesOnlyExpiredReliablePeer()) {
    return 3;
  }
  if (!productionEntryKeepsRunningAfterPeerReliableQueuePressure()) {
    return 4;
  }
  if (!productionEntrySupportsAuthTwoCyclesAndGracefulStop()) {
    return 5;
  }
  return EXIT_SUCCESS;
}
