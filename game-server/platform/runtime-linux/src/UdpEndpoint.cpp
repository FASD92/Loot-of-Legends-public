#include <lol/runtime/linux/UdpEndpoint.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lol::runtime::linux {

#if defined(__linux__)
namespace {

constexpr std::size_t kMaximumUdpPayloadBytes = 65535;

void closeFileDescriptor(int &fileDescriptor) noexcept {
  if (fileDescriptor >= 0) {
    ::close(fileDescriptor);
    fileDescriptor = -1;
  }
}

bool prepareDatagramSocket(int fileDescriptor) noexcept {
  int socketType = 0;
  socklen_t socketTypeBytes = sizeof(socketType);
  if (::getsockopt(fileDescriptor, SOL_SOCKET, SO_TYPE, &socketType,
                   &socketTypeBytes) != 0 ||
      socketType != SOCK_DGRAM) {
    return false;
  }
  const int statusFlags = ::fcntl(fileDescriptor, F_GETFL);
  const int descriptorFlags = ::fcntl(fileDescriptor, F_GETFD);
  return statusFlags >= 0 && descriptorFlags >= 0 &&
         ::fcntl(fileDescriptor, F_SETFL, statusFlags | O_NONBLOCK) == 0 &&
         ::fcntl(fileDescriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0;
}

std::optional<UdpPeerAddress> normalizePeer(const sockaddr_storage &storage,
                                            socklen_t storageBytes) {
  UdpPeerAddress peer{.address = {}, .port = 0, .scopeId = 0};
  if (storage.ss_family == AF_INET && storageBytes >= sizeof(sockaddr_in)) {
    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(&storage);
    peer.address[10] = std::byte{0xff};
    peer.address[11] = std::byte{0xff};
    std::memcpy(peer.address.data() + 12, &ipv4->sin_addr,
                sizeof(ipv4->sin_addr));
    peer.port = ntohs(ipv4->sin_port);
    return peer;
  }
  if (storage.ss_family == AF_INET6 && storageBytes >= sizeof(sockaddr_in6)) {
    const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(&storage);
    std::memcpy(peer.address.data(), &ipv6->sin6_addr, sizeof(ipv6->sin6_addr));
    peer.port = ntohs(ipv6->sin6_port);
    peer.scopeId = ipv6->sin6_scope_id;
    return peer;
  }
  return std::nullopt;
}

} // namespace
#endif

UdpEndpoint::UdpEndpoint(int fileDescriptor) noexcept
    : fileDescriptor_(fileDescriptor) {}

std::optional<UdpEndpoint> UdpEndpoint::adopt(int fileDescriptor) noexcept {
#if defined(__linux__)
  if (fileDescriptor < 0 || !prepareDatagramSocket(fileDescriptor)) {
    closeFileDescriptor(fileDescriptor);
    return std::nullopt;
  }
  return UdpEndpoint{fileDescriptor};
#else
  static_cast<void>(fileDescriptor);
  return std::nullopt;
#endif
}

UdpEndpoint::~UdpEndpoint() {
#if defined(__linux__)
  closeFileDescriptor(fileDescriptor_);
#endif
}

UdpEndpoint::UdpEndpoint(UdpEndpoint &&other) noexcept
    : fileDescriptor_(std::exchange(other.fileDescriptor_, -1)) {}

UdpEndpoint &UdpEndpoint::operator=(UdpEndpoint &&other) noexcept {
  if (this != &other) {
#if defined(__linux__)
    closeFileDescriptor(fileDescriptor_);
#endif
    fileDescriptor_ = std::exchange(other.fileDescriptor_, -1);
  }
  return *this;
}

int UdpEndpoint::fileDescriptor() const noexcept { return fileDescriptor_; }

UdpDrainResult UdpEndpoint::drain(std::size_t maximumDatagrams,
                                  std::size_t maximumDatagramBytes) {
#if defined(__linux__)
  if (fileDescriptor_ < 0 || maximumDatagrams == 0 ||
      maximumDatagramBytes == 0) {
    return {.status = UdpDrainStatus::Failed,
            .datagrams = {},
            .errorNumber = EINVAL};
  }
  const auto bufferBytes =
      std::min(maximumDatagramBytes, kMaximumUdpPayloadBytes);
  std::vector<std::byte> buffer(bufferBytes);
  std::vector<ReceivedUdpDatagram> datagrams;
  datagrams.reserve(maximumDatagrams);

  while (datagrams.size() < maximumDatagrams) {
    sockaddr_storage storage{};
    iovec segment{.iov_base = buffer.data(), .iov_len = buffer.size()};
    msghdr message{.msg_name = &storage,
                   .msg_namelen = sizeof(storage),
                   .msg_iov = &segment,
                   .msg_iovlen = 1,
                   .msg_control = nullptr,
                   .msg_controllen = 0,
                   .msg_flags = 0};
    const auto received = ::recvmsg(fileDescriptor_, &message, MSG_TRUNC);
    if (received >= 0) {
      const auto peer = normalizePeer(storage, message.msg_namelen);
      if (!peer.has_value()) {
        return {.status = UdpDrainStatus::Failed,
                .datagrams = std::move(datagrams),
                .errorNumber = EAFNOSUPPORT};
      }
      const auto receivedBytes = static_cast<std::size_t>(received);
      const auto storedBytes = std::min(receivedBytes, buffer.size());
      datagrams.push_back(ReceivedUdpDatagram{
          .peer = *peer,
          .payload = {buffer.begin(),
                      buffer.begin() +
                          static_cast<std::ptrdiff_t>(storedBytes)},
          .truncated = (message.msg_flags & MSG_TRUNC) != 0 ||
                       receivedBytes > buffer.size(),
      });
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {.status = UdpDrainStatus::Exhausted,
              .datagrams = std::move(datagrams),
              .errorNumber = 0};
    }
    return {.status = UdpDrainStatus::Failed,
            .datagrams = std::move(datagrams),
            .errorNumber = errno};
  }
  return {.status = UdpDrainStatus::BatchLimit,
          .datagrams = std::move(datagrams),
          .errorNumber = 0};
#else
  static_cast<void>(maximumDatagrams);
  static_cast<void>(maximumDatagramBytes);
  return {.status = UdpDrainStatus::Failed, .datagrams = {}, .errorNumber = 0};
#endif
}

} // namespace lol::runtime::linux
