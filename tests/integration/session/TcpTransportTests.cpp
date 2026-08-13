#include <lol/runtime/linux/EpollReactor.hpp>
#include <lol/transport/tcp/TcpConnection.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using lol::transport::tcp::CloseReason;
using lol::transport::tcp::ConnectionPhase;
using lol::transport::tcp::DecodedPreAuthFrame;
using lol::transport::tcp::IoStatus;
using lol::transport::tcp::NormalizedAuthRequest;
using lol::transport::tcp::PreAuthLimits;
using lol::transport::tcp::TcpConnection;
using lol::transport::tcp::TcpSocketIo;

constexpr auto kOpenedAt = std::chrono::steady_clock::time_point{};

PreAuthLimits limits() {
  return PreAuthLimits{
      .maxFrameBytes = 8,
      .maxPreAuthBytes = 16,
      .maxPreAuthReadEvents = 4,
      .preAuthTimeout = 100ms,
      .maxOutboundBytes = 4,
  };
}

std::vector<std::byte> frame(std::initializer_list<std::uint8_t> payload) {
  const auto size = static_cast<std::uint32_t>(payload.size());
  std::vector<std::byte> bytes{
      static_cast<std::byte>((size >> 24U) & 0xffU),
      static_cast<std::byte>((size >> 16U) & 0xffU),
      static_cast<std::byte>((size >> 8U) & 0xffU),
      static_cast<std::byte>(size & 0xffU),
  };
  for (const std::uint8_t value : payload) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

DecodedPreAuthFrame decodeForTest(std::span<const std::byte> payload) {
  if (payload.empty()) {
    return DecodedPreAuthFrame::malformed();
  }
  if (payload.front() != std::byte{1}) {
    return DecodedPreAuthFrame::otherMessage();
  }
  return DecodedPreAuthFrame::authenticate(NormalizedAuthRequest{
      .requestId = 7,
      .protocolMajor = 1,
      .credential = "credential",
  });
}

bool partialFrameEmitsOneNormalizedRequest() {
  TcpConnection connection{limits(), kOpenedAt};
  std::vector<NormalizedAuthRequest> requests;
  const auto bytes = frame({1});

  connection.onBytes(std::span{bytes}.first(2), kOpenedAt + 1ms, decodeForTest,
                     [&requests](NormalizedAuthRequest request) {
                       requests.push_back(std::move(request));
                     });
  if (connection.phase() != ConnectionPhase::ConnectedUnauthenticated ||
      !requests.empty()) {
    return false;
  }

  connection.onBytes(std::span{bytes}.subspan(2), kOpenedAt + 2ms,
                     decodeForTest, [&requests](NormalizedAuthRequest request) {
                       requests.push_back(std::move(request));
                     });
  return connection.phase() == ConnectionPhase::AwaitingClaim &&
         requests.size() == 1 && requests.front().requestId == 7 &&
         requests.front().credential == "credential";
}

bool oversizedAndPreAuthCommandsCloseWithoutEmission() {
  TcpConnection oversized{limits(), kOpenedAt};
  const std::array oversizedHeader{std::byte{0}, std::byte{0}, std::byte{0},
                                   std::byte{9}};
  std::size_t emitted = 0;
  const auto sink = [&emitted](NormalizedAuthRequest) { ++emitted; };
  oversized.onBytes(oversizedHeader, kOpenedAt + 1ms, decodeForTest, sink);

  TcpConnection other{limits(), kOpenedAt};
  const auto otherFrame = frame({2});
  other.onBytes(otherFrame, kOpenedAt + 1ms, decodeForTest, sink);

  TcpConnection awaitingClaim{limits(), kOpenedAt};
  const auto authFrame = frame({1});
  awaitingClaim.onBytes(authFrame, kOpenedAt + 1ms, decodeForTest,
                        [](NormalizedAuthRequest) {});
  awaitingClaim.onBytes(otherFrame, kOpenedAt + 2ms, decodeForTest,
                        [](NormalizedAuthRequest) {});

  return oversized.closeReason() == CloseReason::FrameTooLarge &&
         other.closeReason() == CloseReason::PreAuthCommand &&
         awaitingClaim.closeReason() == CloseReason::PreAuthCommand &&
         emitted == 0;
}

bool preAuthBudgetsAreBounded() {
  PreAuthLimits byteLimits = limits();
  byteLimits.maxPreAuthBytes = 4;
  TcpConnection bytes{byteLimits, kOpenedAt};
  const auto authFrame = frame({1});
  bytes.onBytes(authFrame, kOpenedAt + 1ms, decodeForTest,
                [](NormalizedAuthRequest) {});

  PreAuthLimits rateLimits = limits();
  rateLimits.maxPreAuthReadEvents = 1;
  TcpConnection rate{rateLimits, kOpenedAt};
  rate.onBytes(std::span{authFrame}.first(1), kOpenedAt + 1ms, decodeForTest,
               [](NormalizedAuthRequest) {});
  rate.onBytes(std::span{authFrame}.subspan(1, 1), kOpenedAt + 2ms,
               decodeForTest, [](NormalizedAuthRequest) {});

  TcpConnection timeout{limits(), kOpenedAt};
  timeout.onBytes(authFrame, kOpenedAt + 1ms, decodeForTest,
                  [](NormalizedAuthRequest) {});
  timeout.onTimer(kOpenedAt + 100ms);

  return bytes.closeReason() == CloseReason::PreAuthByteBudget &&
         rate.closeReason() == CloseReason::PreAuthRateBudget &&
         timeout.closeReason() == CloseReason::PreAuthTimeout;
}

bool slowWriterAndCloseLifecycleAreBounded() {
  TcpConnection connection{limits(), kOpenedAt};
  const std::array full{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const std::array extra{std::byte{5}};

  if (!connection.queueOutbound(full) || connection.queueOutbound(extra) ||
      connection.closeReason() != CloseReason::SlowWriter) {
    return false;
  }
  if (connection.beginClose(CloseReason::PeerClosed) ||
      !connection.markClosed() || connection.markClosed()) {
    return false;
  }
  return connection.phase() == ConnectionPhase::Closed &&
         connection.closeReason() == CloseReason::SlowWriter;
}

bool nonBlockingSocketIoHandlesAcceptReadAndWrite() {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0 || !TcpSocketIo::setNonBlocking(listener)) {
    if (listener >= 0) {
      ::close(listener);
    }
    return false;
  }

  const int reuse = 1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  socklen_t addressSize = sizeof(address);
  const bool listenerReady =
      ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ==
          0 &&
      ::bind(listener, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) == 0 &&
      ::listen(listener, 1) == 0 &&
      ::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                    &addressSize) == 0;
  if (!listenerReady) {
    ::close(listener);
    return false;
  }

  const auto emptyAccept = TcpSocketIo::acceptNonBlocking(listener);
  const int client = ::socket(AF_INET, SOCK_STREAM, 0);
  if (emptyAccept.status != IoStatus::WouldBlock || client < 0) {
    if (client >= 0) {
      ::close(client);
    }
    ::close(listener);
    return false;
  }
  if (::connect(client, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
    ::close(client);
    ::close(listener);
    return false;
  }
  pollfd acceptReady{.fd = listener, .events = POLLIN, .revents = 0};
  if (::poll(&acceptReady, 1, 1000) != 1) {
    ::close(client);
    ::close(listener);
    return false;
  }

  const auto accepted = TcpSocketIo::acceptNonBlocking(listener);
  if (accepted.status != IoStatus::Progress || accepted.fileDescriptor < 0 ||
      (::fcntl(accepted.fileDescriptor, F_GETFL, 0) & O_NONBLOCK) == 0) {
    if (accepted.fileDescriptor >= 0) {
      ::close(accepted.fileDescriptor);
    }
    ::close(client);
    ::close(listener);
    return false;
  }

  std::array<std::byte, 1> received{};
  const auto initiallyEmpty =
      TcpSocketIo::receiveNonBlocking(accepted.fileDescriptor, received);
  const std::array sent{std::byte{0x5a}};
  const auto write = TcpSocketIo::sendNonBlocking(client, sent);
  pollfd readReady{
      .fd = accepted.fileDescriptor, .events = POLLIN, .revents = 0};
  const int pollResult = ::poll(&readReady, 1, 1000);
  const auto read =
      TcpSocketIo::receiveNonBlocking(accepted.fileDescriptor, received);

  ::close(accepted.fileDescriptor);
  ::close(client);
  ::close(listener);
  return initiallyEmpty.status == IoStatus::WouldBlock &&
         write.status == IoStatus::Progress && write.bytes == 1 &&
         pollResult == 1 && read.status == IoStatus::Progress &&
         read.bytes == 1 && received.front() == std::byte{0x5a};
}

bool linuxReadinessPrimitivesAreUsable() {
  lol::runtime::linux::EpollReactor reactor{8};
  if (!lol::runtime::linux::EpollReactor::supported()) {
    return !reactor.valid() && reactor.wait(0ms).empty();
  }
  if (!reactor.valid() || !reactor.wake()) {
    return false;
  }
  const auto wakeEvents = reactor.wait(50ms);
  const bool woke = std::ranges::any_of(
      wakeEvents, [](const lol::runtime::linux::ReadyEvent &event) {
        return event.kind == lol::runtime::linux::ReadyKind::Wake;
      });
  if (!woke || !reactor.armTimer(1ms)) {
    return false;
  }
  const auto timerEvents = reactor.wait(100ms);
  const bool timerFired = std::ranges::any_of(
      timerEvents, [](const lol::runtime::linux::ReadyEvent &event) {
        return event.kind == lol::runtime::linux::ReadyKind::Timer;
      });
  std::array<int, 2> sockets{-1, -1};
  if (!timerFired ||
      ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0 ||
      !reactor.watch(sockets.front())) {
    if (sockets.front() >= 0) {
      ::close(sockets.front());
    }
    if (sockets.back() >= 0) {
      ::close(sockets.back());
    }
    return false;
  }

  const std::uint8_t value = 1;
  const bool wrote = ::write(sockets.back(), &value, sizeof(value)) ==
                     static_cast<ssize_t>(sizeof(value));
  const auto socketEvents = reactor.wait(100ms);
  const bool socketReady = std::ranges::any_of(
      socketEvents, [&sockets](const lol::runtime::linux::ReadyEvent &event) {
        return event.kind == lol::runtime::linux::ReadyKind::Socket &&
               event.fileDescriptor == sockets.front() && event.readable;
      });
  const bool unwatched = reactor.unwatch(sockets.front());
  ::close(sockets.front());
  ::close(sockets.back());
  return wrote && socketReady && unwatched;
}

} // namespace

int main() {
  if (!partialFrameEmitsOneNormalizedRequest()) {
    return 1;
  }
  if (!oversizedAndPreAuthCommandsCloseWithoutEmission()) {
    return 2;
  }
  if (!preAuthBudgetsAreBounded()) {
    return 3;
  }
  if (!slowWriterAndCloseLifecycleAreBounded()) {
    return 4;
  }
  if (!nonBlockingSocketIoHandlesAcceptReadAndWrite()) {
    return 5;
  }
  if (!linuxReadinessPrimitivesAreUsable()) {
    return 6;
  }
  return EXIT_SUCCESS;
}
