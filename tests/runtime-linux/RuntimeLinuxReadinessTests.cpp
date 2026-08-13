#include <lol/runtime/linux/EpollReactor.hpp>
#include <lol/runtime/linux/UdpEndpoint.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#if defined(__linux__)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using lol::runtime::linux::EpollReactor;
using lol::runtime::linux::ReadyEvent;
using lol::runtime::linux::ReadyKind;
using lol::runtime::linux::UdpDrainStatus;
using lol::runtime::linux::UdpEndpoint;

#if defined(__linux__)

class OwnedFileDescriptor final {
public:
  explicit OwnedFileDescriptor(int value) noexcept : value_(value) {}
  ~OwnedFileDescriptor() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }

  OwnedFileDescriptor(const OwnedFileDescriptor &) = delete;
  OwnedFileDescriptor &operator=(const OwnedFileDescriptor &) = delete;
  OwnedFileDescriptor(OwnedFileDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}

  [[nodiscard]] int get() const noexcept { return value_; }

private:
  int value_;
};

struct UdpFixture final {
  UdpEndpoint endpoint;
  OwnedFileDescriptor sender;
  sockaddr_in destination;
};

std::optional<UdpFixture> makeUdpFixture() {
  const int receiver = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (receiver < 0) {
    return std::nullopt;
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(receiver, reinterpret_cast<const sockaddr *>(&local),
             sizeof(local)) != 0) {
    ::close(receiver);
    return std::nullopt;
  }
  socklen_t localBytes = sizeof(local);
  if (::getsockname(receiver, reinterpret_cast<sockaddr *>(&local),
                    &localBytes) != 0) {
    ::close(receiver);
    return std::nullopt;
  }
  auto endpoint = UdpEndpoint::adopt(receiver);
  const int sender = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (!endpoint.has_value() || sender < 0) {
    if (sender >= 0) {
      ::close(sender);
    }
    return std::nullopt;
  }
  return UdpFixture{.endpoint = std::move(*endpoint),
                    .sender = OwnedFileDescriptor{sender},
                    .destination = local};
}

bool sendDatagram(const UdpFixture &fixture, std::string_view payload) {
  return ::sendto(fixture.sender.get(), payload.data(), payload.size(), 0,
                  reinterpret_cast<const sockaddr *>(&fixture.destination),
                  sizeof(fixture.destination)) ==
         static_cast<ssize_t>(payload.size());
}

bool hasKind(std::span<const ReadyEvent> events, ReadyKind kind,
             int fileDescriptor = -1) {
  return std::ranges::any_of(events, [kind, fileDescriptor](const auto &event) {
    return event.kind == kind &&
           (fileDescriptor < 0 || event.fileDescriptor == fileDescriptor);
  });
}

bool drainsUdpUntilWouldBlock() {
  auto fixture = makeUdpFixture();
  if (!fixture.has_value() || !sendDatagram(*fixture, "one") ||
      !sendDatagram(*fixture, "two")) {
    return false;
  }
  const auto limited = fixture->endpoint.drain(1, 1200);
  const auto drained = fixture->endpoint.drain(8, 1200);
  const auto empty = fixture->endpoint.drain(8, 1200);
  const std::string oversized(1201, 'x');
  if (limited.status != UdpDrainStatus::BatchLimit ||
      limited.datagrams.size() != 1 ||
      drained.status != UdpDrainStatus::Exhausted ||
      drained.datagrams.size() != 1 ||
      empty.status != UdpDrainStatus::Exhausted || !empty.datagrams.empty() ||
      !sendDatagram(*fixture, oversized)) {
    return false;
  }
  const auto truncated = fixture->endpoint.drain(1, 1200);
  return truncated.status == UdpDrainStatus::BatchLimit &&
         truncated.datagrams.size() == 1 &&
         truncated.datagrams.front().payload.size() == 1200 &&
         truncated.datagrams.front().truncated;
}

bool reactorHandlesSocketTimerWakeAndUnwatch() {
  auto fixture = makeUdpFixture();
  EpollReactor reactor{8};
  if (!fixture.has_value() || !reactor.valid() ||
      !reactor.watch(fixture->endpoint.fileDescriptor()) ||
      !sendDatagram(*fixture, "ready")) {
    return false;
  }
  const auto socketEvents = reactor.wait(100ms);
  if (!hasKind(socketEvents, ReadyKind::Socket,
               fixture->endpoint.fileDescriptor()) ||
      fixture->endpoint.drain(8, 1200).datagrams.size() != 1 ||
      !reactor.armTimer(1ms) ||
      !hasKind(reactor.wait(100ms), ReadyKind::Timer) || !reactor.wake() ||
      !hasKind(reactor.wait(100ms), ReadyKind::Wake) ||
      !reactor.unwatch(fixture->endpoint.fileDescriptor()) ||
      !sendDatagram(*fixture, "stale")) {
    return false;
  }
  return !hasKind(reactor.wait(10ms), ReadyKind::Socket,
                  fixture->endpoint.fileDescriptor());
}

#endif

} // namespace

int main() {
#if defined(__linux__)
  return drainsUdpUntilWouldBlock() && reactorHandlesSocketTimerWakeAndUnwatch()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
#else
  return EpollReactor::supported() ? EXIT_FAILURE : EXIT_SUCCESS;
#endif
}
