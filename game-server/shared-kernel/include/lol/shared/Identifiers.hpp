#pragma once

#include <array>
#include <compare>
#include <cstdint>

namespace lol::shared {

class AccountId final {
public:
  using Bytes = std::array<std::uint8_t, 16>;

  explicit AccountId(Bytes bytes) noexcept;

  [[nodiscard]] const Bytes &bytes() const noexcept;
  auto operator<=>(const AccountId &) const = default;

private:
  Bytes bytes_;
};

class SessionId final {
public:
  explicit SessionId(std::uint64_t value) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept;
  auto operator<=>(const SessionId &) const = default;

private:
  std::uint64_t value_;
};

class SessionGeneration final {
public:
  explicit SessionGeneration(std::uint64_t value) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept;
  auto operator<=>(const SessionGeneration &) const = default;

private:
  std::uint64_t value_;
};

class RequestId final {
public:
  explicit RequestId(std::uint64_t value) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept;
  auto operator<=>(const RequestId &) const = default;

private:
  std::uint64_t value_;
};

class RoomId final {
public:
  explicit RoomId(std::uint64_t value) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept;
  auto operator<=>(const RoomId &) const = default;

private:
  std::uint64_t value_;
};

class BattleInstanceId final {
public:
  explicit BattleInstanceId(std::uint64_t value) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept;
  auto operator<=>(const BattleInstanceId &) const = default;

private:
  std::uint64_t value_;
};

} // namespace lol::shared
