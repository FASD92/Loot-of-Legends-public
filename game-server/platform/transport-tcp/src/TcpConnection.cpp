#include <lol/transport/tcp/TcpConnection.hpp>

#include <algorithm>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lol::transport::tcp {
namespace {

constexpr std::size_t kFrameHeaderBytes = 4;

std::uint32_t frameLength(std::span<const std::byte> header) noexcept {
  std::uint32_t length = 0;
  for (const std::byte value : header.first(kFrameHeaderBytes)) {
    length = (length << 8U) | std::to_integer<std::uint32_t>(value);
  }
  return length;
}

IoStatus statusForError(int errorNumber) noexcept {
  if (errorNumber == EAGAIN || errorNumber == EWOULDBLOCK) {
    return IoStatus::WouldBlock;
  }
  if (errorNumber == EINTR) {
    return IoStatus::Interrupted;
  }
  return IoStatus::Failed;
}

} // namespace

DecodedPreAuthFrame
DecodedPreAuthFrame::authenticate(NormalizedAuthRequest request) {
  return DecodedPreAuthFrame{Kind::Authenticate, std::move(request)};
}

DecodedPreAuthFrame DecodedPreAuthFrame::otherMessage() {
  return DecodedPreAuthFrame{Kind::OtherMessage, std::nullopt};
}

DecodedPreAuthFrame DecodedPreAuthFrame::malformed() {
  return DecodedPreAuthFrame{Kind::Malformed, std::nullopt};
}

TcpConnection::TcpConnection(PreAuthLimits limits,
                             std::chrono::steady_clock::time_point openedAt)
    : limits_(limits), openedAt_(openedAt) {
  inbound_.reserve(std::min(limits_.maxPreAuthBytes,
                            limits_.maxFrameBytes + kFrameHeaderBytes));
  outbound_.reserve(limits_.maxOutboundBytes);
}

void TcpConnection::onBytes(std::span<const std::byte> bytes,
                            std::chrono::steady_clock::time_point now,
                            const PreAuthDecoder &decoder,
                            const AuthRequestSink &sink) {
  if (bytes.empty()) {
    return;
  }
  onTimer(now);
  if (phase_ == ConnectionPhase::AwaitingClaim) {
    closeFor(CloseReason::PreAuthCommand);
    return;
  }
  if (phase_ != ConnectionPhase::ConnectedUnauthenticated) {
    return;
  }

  ++preAuthReadEvents_;
  if (preAuthReadEvents_ > limits_.maxPreAuthReadEvents) {
    closeFor(CloseReason::PreAuthRateBudget);
    return;
  }
  if (bytes.size() > limits_.maxPreAuthBytes -
                         std::min(preAuthBytes_, limits_.maxPreAuthBytes)) {
    closeFor(CloseReason::PreAuthByteBudget);
    return;
  }
  preAuthBytes_ += bytes.size();
  inbound_.insert(inbound_.end(), bytes.begin(), bytes.end());
  decodeBufferedFrame(decoder, sink);
}

void TcpConnection::onTimer(
    std::chrono::steady_clock::time_point now) noexcept {
  if ((phase_ == ConnectionPhase::ConnectedUnauthenticated ||
       phase_ == ConnectionPhase::AwaitingClaim) &&
      now >= openedAt_ + limits_.preAuthTimeout) {
    closeFor(CloseReason::PreAuthTimeout);
  }
}

bool TcpConnection::markAuthenticated() noexcept {
  if (phase_ != ConnectionPhase::AwaitingClaim) {
    return false;
  }
  phase_ = ConnectionPhase::Authenticated;
  return true;
}

bool TcpConnection::beginClose(CloseReason reason) noexcept {
  if (phase_ == ConnectionPhase::Closing || phase_ == ConnectionPhase::Closed) {
    return false;
  }
  phase_ = ConnectionPhase::Closing;
  closeReason_ = reason;
  return true;
}

bool TcpConnection::markClosed() noexcept {
  if (phase_ != ConnectionPhase::Closing) {
    return false;
  }
  phase_ = ConnectionPhase::Closed;
  return true;
}

bool TcpConnection::queueOutbound(std::span<const std::byte> bytes) {
  if (phase_ == ConnectionPhase::Closed) {
    return false;
  }
  if (bytes.size() > limits_.maxOutboundBytes -
                         std::min(outbound_.size(), limits_.maxOutboundBytes)) {
    closeFor(CloseReason::SlowWriter);
    return false;
  }
  outbound_.insert(outbound_.end(), bytes.begin(), bytes.end());
  return true;
}

std::span<const std::byte> TcpConnection::pendingOutbound() const noexcept {
  return outbound_;
}

bool TcpConnection::consumeOutbound(std::size_t bytes) noexcept {
  if (bytes > outbound_.size()) {
    return false;
  }
  using Difference = std::vector<std::byte>::difference_type;
  outbound_.erase(outbound_.begin(),
                  outbound_.begin() + static_cast<Difference>(bytes));
  return true;
}

ConnectionPhase TcpConnection::phase() const noexcept { return phase_; }

CloseReason TcpConnection::closeReason() const noexcept { return closeReason_; }

void TcpConnection::closeFor(CloseReason reason) noexcept {
  static_cast<void>(beginClose(reason));
}

void TcpConnection::decodeBufferedFrame(const PreAuthDecoder &decoder,
                                        const AuthRequestSink &sink) {
  if (inbound_.size() < kFrameHeaderBytes) {
    return;
  }
  const std::uint32_t length = frameLength(inbound_);
  if (length == 0) {
    closeFor(CloseReason::MalformedFrame);
    return;
  }
  if (length > limits_.maxFrameBytes) {
    closeFor(CloseReason::FrameTooLarge);
    return;
  }
  const std::size_t completeFrameBytes =
      kFrameHeaderBytes + static_cast<std::size_t>(length);
  if (inbound_.size() < completeFrameBytes) {
    return;
  }
  if (inbound_.size() != completeFrameBytes) {
    closeFor(CloseReason::PreAuthCommand);
    return;
  }

  const auto payload =
      std::span<const std::byte>{inbound_}.subspan(kFrameHeaderBytes);
  DecodedPreAuthFrame decoded = decoder(payload);
  if (decoded.kind == DecodedPreAuthFrame::Kind::OtherMessage) {
    closeFor(CloseReason::PreAuthCommand);
    return;
  }
  if (decoded.kind == DecodedPreAuthFrame::Kind::Malformed ||
      !decoded.request.has_value()) {
    closeFor(CloseReason::MalformedFrame);
    return;
  }

  phase_ = ConnectionPhase::AwaitingClaim;
  inbound_.clear();
  sink(std::move(*decoded.request));
}

bool TcpSocketIo::setNonBlocking(int fileDescriptor) noexcept {
  const int currentFlags = ::fcntl(fileDescriptor, F_GETFL, 0);
  if (currentFlags < 0 ||
      ::fcntl(fileDescriptor, F_SETFL, currentFlags | O_NONBLOCK) < 0) {
    return false;
  }
  const int descriptorFlags = ::fcntl(fileDescriptor, F_GETFD, 0);
  if (descriptorFlags < 0 ||
      ::fcntl(fileDescriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
    return false;
  }
#if defined(__APPLE__)
  const int noSignal = 1;
  if (::setsockopt(fileDescriptor, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                   sizeof(noSignal)) < 0) {
    return false;
  }
#endif
  return true;
}

AcceptResult
TcpSocketIo::acceptNonBlocking(int listenerFileDescriptor) noexcept {
#if defined(__linux__)
  const int accepted = ::accept4(listenerFileDescriptor, nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  const int accepted = ::accept(listenerFileDescriptor, nullptr, nullptr);
#endif
  if (accepted < 0) {
    const int errorNumber = errno;
    return AcceptResult{statusForError(errorNumber), -1, errorNumber};
  }
#if !defined(__linux__)
  if (!setNonBlocking(accepted)) {
    const int errorNumber = errno;
    ::close(accepted);
    return AcceptResult{IoStatus::Failed, -1, errorNumber};
  }
#endif
  return AcceptResult{IoStatus::Progress, accepted, 0};
}

IoResult
TcpSocketIo::receiveNonBlocking(int fileDescriptor,
                                std::span<std::byte> destination) noexcept {
  if (destination.empty()) {
    return IoResult{IoStatus::Progress, 0, 0};
  }
  const auto received = ::recv(fileDescriptor, destination.data(),
                               destination.size(), MSG_DONTWAIT);
  if (received > 0) {
    return IoResult{IoStatus::Progress, static_cast<std::size_t>(received), 0};
  }
  if (received == 0) {
    return IoResult{IoStatus::PeerClosed, 0, 0};
  }
  const int errorNumber = errno;
  return IoResult{statusForError(errorNumber), 0, errorNumber};
}

IoResult
TcpSocketIo::sendNonBlocking(int fileDescriptor,
                             std::span<const std::byte> source) noexcept {
  int flags = MSG_DONTWAIT;
#if defined(__linux__)
  flags |= MSG_NOSIGNAL;
#endif
  const auto sent = ::send(fileDescriptor, source.data(), source.size(), flags);
  if (sent >= 0) {
    return IoResult{IoStatus::Progress, static_cast<std::size_t>(sent), 0};
  }
  const int errorNumber = errno;
  return IoResult{statusForError(errorNumber), 0, errorNumber};
}

} // namespace lol::transport::tcp
