#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lol::transport::tcp {

enum class ConnectionPhase {
  ConnectedUnauthenticated,
  AwaitingClaim,
  Authenticated,
  Closing,
  Closed,
};

enum class CloseReason {
  None,
  MalformedFrame,
  FrameTooLarge,
  PreAuthCommand,
  PreAuthByteBudget,
  PreAuthRateBudget,
  PreAuthTimeout,
  SlowWriter,
  PeerClosed,
  IoError,
  AuthenticationRejected,
  SessionReplaced,
};

struct PreAuthLimits final {
  std::size_t maxFrameBytes;
  std::size_t maxPreAuthBytes;
  std::size_t maxPreAuthReadEvents;
  std::chrono::milliseconds preAuthTimeout;
  std::size_t maxOutboundBytes;
};

struct NormalizedAuthRequest final {
  std::uint64_t requestId;
  std::uint16_t protocolMajor;
  std::string credential;
};

struct DecodedPreAuthFrame final {
  enum class Kind { Authenticate, OtherMessage, Malformed };

  Kind kind;
  std::optional<NormalizedAuthRequest> request;

  [[nodiscard]] static DecodedPreAuthFrame
  authenticate(NormalizedAuthRequest request);
  [[nodiscard]] static DecodedPreAuthFrame otherMessage();
  [[nodiscard]] static DecodedPreAuthFrame malformed();
};

using PreAuthDecoder =
    std::function<DecodedPreAuthFrame(std::span<const std::byte>)>;
using AuthRequestSink = std::function<void(NormalizedAuthRequest)>;

class TcpConnection final {
public:
  TcpConnection(PreAuthLimits limits,
                std::chrono::steady_clock::time_point openedAt);

  void onBytes(std::span<const std::byte> bytes,
               std::chrono::steady_clock::time_point now,
               const PreAuthDecoder &decoder, const AuthRequestSink &sink);
  void onTimer(std::chrono::steady_clock::time_point now) noexcept;

  [[nodiscard]] bool markAuthenticated() noexcept;
  [[nodiscard]] bool beginClose(CloseReason reason) noexcept;
  [[nodiscard]] bool markClosed() noexcept;

  [[nodiscard]] bool queueOutbound(std::span<const std::byte> bytes);
  [[nodiscard]] std::span<const std::byte> pendingOutbound() const noexcept;
  [[nodiscard]] bool consumeOutbound(std::size_t bytes) noexcept;

  [[nodiscard]] ConnectionPhase phase() const noexcept;
  [[nodiscard]] CloseReason closeReason() const noexcept;

private:
  void closeFor(CloseReason reason) noexcept;
  void decodeBufferedFrame(const PreAuthDecoder &decoder,
                           const AuthRequestSink &sink);

  PreAuthLimits limits_;
  std::chrono::steady_clock::time_point openedAt_;
  ConnectionPhase phase_{ConnectionPhase::ConnectedUnauthenticated};
  CloseReason closeReason_{CloseReason::None};
  std::size_t preAuthBytes_{0};
  std::size_t preAuthReadEvents_{0};
  std::vector<std::byte> inbound_;
  std::vector<std::byte> outbound_;
};

enum class IoStatus { Progress, WouldBlock, Interrupted, PeerClosed, Failed };

struct IoResult final {
  IoStatus status;
  std::size_t bytes;
  int errorNumber;
};

struct AcceptResult final {
  IoStatus status;
  int fileDescriptor;
  int errorNumber;
};

class TcpSocketIo final {
public:
  [[nodiscard]] static bool setNonBlocking(int fileDescriptor) noexcept;
  [[nodiscard]] static AcceptResult
  acceptNonBlocking(int listenerFileDescriptor) noexcept;
  [[nodiscard]] static IoResult
  receiveNonBlocking(int fileDescriptor,
                     std::span<std::byte> destination) noexcept;
  [[nodiscard]] static IoResult
  sendNonBlocking(int fileDescriptor,
                  std::span<const std::byte> source) noexcept;
};

} // namespace lol::transport::tcp
