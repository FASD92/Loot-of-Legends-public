#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lol::runtime::linux {

struct UdpPeerAddress final {
  std::array<std::byte, 16> address;
  std::uint16_t port;
  std::uint32_t scopeId;

  bool operator==(const UdpPeerAddress &) const = default;
};

struct ReceivedUdpDatagram final {
  UdpPeerAddress peer;
  std::vector<std::byte> payload;
  bool truncated;
};

enum class UdpDrainStatus : std::uint8_t {
  Exhausted,
  BatchLimit,
  Failed,
};

struct UdpDrainResult final {
  UdpDrainStatus status;
  std::vector<ReceivedUdpDatagram> datagrams;
  int errorNumber;
};

class UdpEndpoint final {
public:
  [[nodiscard]] static std::optional<UdpEndpoint>
  adopt(int fileDescriptor) noexcept;
  ~UdpEndpoint();

  UdpEndpoint(const UdpEndpoint &) = delete;
  UdpEndpoint &operator=(const UdpEndpoint &) = delete;
  UdpEndpoint(UdpEndpoint &&other) noexcept;
  UdpEndpoint &operator=(UdpEndpoint &&other) noexcept;

  [[nodiscard]] int fileDescriptor() const noexcept;
  [[nodiscard]] UdpDrainResult drain(std::size_t maximumDatagrams,
                                     std::size_t maximumDatagramBytes);

private:
  explicit UdpEndpoint(int fileDescriptor) noexcept;

  int fileDescriptor_{-1};
};

} // namespace lol::runtime::linux
