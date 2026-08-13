#include <lol/transport/rudp/RudpCodec.hpp>
#include <lol/transport/rudp/RudpCombatCodec.hpp>
#include <lol/transport/rudp/RudpHeader.hpp>
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
using lol::transport::rudp::RudpAttackIntent;
using lol::transport::rudp::RudpBindAccepted;
using lol::transport::rudp::RudpBindHello;
using lol::transport::rudp::RudpCombatCodec;
using lol::transport::rudp::RudpCombatMessage;
using lol::transport::rudp::RudpCommandId;
using lol::transport::rudp::RudpControlCodec;
using lol::transport::rudp::RudpControlCodecError;
using lol::transport::rudp::RudpControlMessage;
using lol::transport::rudp::RudpFlag;
using lol::transport::rudp::RudpHeader;
using lol::transport::rudp::RudpHeaderCodec;
using lol::transport::rudp::RudpHeaderError;
using lol::transport::rudp::RudpHeartbeat;
using lol::transport::rudp::RudpPeerDelivery;
using lol::transport::tcp::ArenaLoadComplete;
using lol::transport::tcp::AuthenticateGameSession;
using lol::transport::tcp::BattleLoadClientMessage;
using lol::transport::tcp::BattleLoadProtocolCodec;
using lol::transport::tcp::CreateRoom;
using lol::transport::tcp::HostStartRequest;
using lol::transport::tcp::JoinRoom;
using lol::transport::tcp::LeaveRoom;
using lol::transport::tcp::LobbyRoomClientMessage;
using lol::transport::tcp::LobbyRoomProtocolCodec;
using lol::transport::tcp::RequestRudpBindCapability;
using lol::transport::tcp::SessionProtocolCodec;
using lol::transport::tcp::SetReady;
using lol::transport::tcp::Welcome;

constexpr std::uint32_t kWelcomeMessageId = 2u;
constexpr std::uint32_t kSessionReplacedMessageId = 4u;
constexpr std::uint32_t kRoomCommandResponseMessageId = 12u;
constexpr std::uint32_t kRoomDetailMessageId = 13u;
constexpr std::uint32_t kBattleCommandResponseMessageId = 15u;
constexpr std::uint32_t kArenaLoadEntryMessageId = 16u;
constexpr std::uint32_t kArenaGameplayStartMessageId = 18u;
constexpr std::uint32_t kRudpBindCapabilityMessageId = 21u;
constexpr std::uint32_t kFinalResultMessageId = 36u;

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

std::string
configText(const TemporaryDirectory &directory, std::uint16_t tcpPort = 0u,
           std::uint16_t udpPort = 0u,
           std::optional<std::uint16_t> metricsPort = std::nullopt) {
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
      "test_meta_fixture=true\n";
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

bool startBattle(TcpClient &host, TcpClient &member, std::uint64_t battleId,
                 std::uint64_t requestBase) {
  if (!sendLobby(host, SetReady{.requestId = requestBase, .ready = true}) ||
      !host.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !sendLobby(member,
                 SetReady{.requestId = requestBase + 1u, .ready = true}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !sendBattle(host, HostStartRequest{.requestId = requestBase + 2u}) ||
      !host.take(kBattleCommandResponseMessageId, 2s).has_value()) {
    return false;
  }
  const auto entry = host.take(kArenaLoadEntryMessageId, 2s);
  if (!entry.has_value() || readU64(*entry, 17u) != battleId ||
      !sendBattle(host, ArenaLoadComplete{.requestId = requestBase + 3u,
                                          .roomId = 1u,
                                          .battleInstanceId = battleId}) ||
      !host.take(kBattleCommandResponseMessageId, 2s).has_value() ||
      !sendBattle(member, ArenaLoadComplete{.requestId = requestBase + 4u,
                                            .roomId = 1u,
                                            .battleInstanceId = battleId}) ||
      !member.take(kBattleCommandResponseMessageId, 2s).has_value() ||
      !host.take(kArenaGameplayStartMessageId, 2s).has_value()) {
    return false;
  }
  return true;
}

bool runBattleCycle(TcpClient &host, TcpClient &member, UdpClient &hostUdp,
                    UdpClient &memberUdp, std::uint64_t battleId,
                    std::uint64_t requestBase) {
  if (!startBattle(host, member, battleId, requestBase)) {
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

template <class FailureDriver>
bool productionEntryClosesOnlyFailedPeer(std::string_view roomTitle,
                                         std::chrono::milliseconds closeTimeout,
                                         FailureDriver driveFailure) {
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
      !bindUdp(memberUdp, *ports, *memberWelcome, *memberCapability) ||
      !sendLobby(host, CreateRoom{.requestId = 5u,
                                  .title = std::string{roomTitle},
                                  .capacity = 2u}) ||
      !host.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !host.take(kRoomDetailMessageId, 2s).has_value() ||
      !sendLobby(member, JoinRoom{.requestId = 6u, .roomId = 1u}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kRoomDetailMessageId, 2s).has_value() ||
      !startBattle(host, member, 1u, 10u)) {
    return false;
  }
  if (!driveFailure(hostUdp, memberUdp)) {
    return false;
  }
  const bool hostClosed = host.waitForClose(closeTimeout);
  const bool processAlive = child.running();
  const bool memberContinues =
      sendLobby(member, LeaveRoom{.requestId = 100u}) &&
      member.take(kRoomCommandResponseMessageId, 2s).has_value();
  const auto exitCode = child.stop(SIGTERM, 8s);
  return hostClosed && processAlive && memberContinues &&
         exitCode.has_value() && *exitCode == 0;
}

bool productionEntryClosesOnlyExpiredReliablePeer() {
  return productionEntryClosesOnlyFailedPeer(
      "expiry-room", 2s, [](UdpClient &host, UdpClient &member) {
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
  return productionEntryClosesOnlyFailedPeer(
      "pressure-room", 3s, [](UdpClient &host, UdpClient &member) {
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
      !bindUdp(memberUdp, *ports, *memberWelcome, *memberCapability) ||
      !sendLobby(
          host,
          CreateRoom{.requestId = 6u, .title = "entry-room", .capacity = 2u}) ||
      !host.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !host.take(kRoomDetailMessageId, 2s).has_value() ||
      !sendLobby(member, JoinRoom{.requestId = 7u, .roomId = 1u}) ||
      !member.take(kRoomCommandResponseMessageId, 2s).has_value() ||
      !member.take(kRoomDetailMessageId, 2s).has_value() ||
      !runBattleCycle(host, member, hostUdp, memberUdp, 1u, 10u) ||
      !runBattleCycle(host, member, hostUdp, memberUdp, 2u, 20u)) {
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

} // namespace

int main() {
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
