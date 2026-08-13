#include <lol/shared/Identifiers.hpp>

#include <utility>

namespace lol::shared {

AccountId::AccountId(Bytes bytes) noexcept : bytes_(std::move(bytes)) {}

const AccountId::Bytes &AccountId::bytes() const noexcept { return bytes_; }

SessionId::SessionId(std::uint64_t value) noexcept : value_(value) {}

std::uint64_t SessionId::value() const noexcept { return value_; }

SessionGeneration::SessionGeneration(std::uint64_t value) noexcept
    : value_(value) {}

std::uint64_t SessionGeneration::value() const noexcept { return value_; }

RequestId::RequestId(std::uint64_t value) noexcept : value_(value) {}

std::uint64_t RequestId::value() const noexcept { return value_; }

RoomId::RoomId(std::uint64_t value) noexcept : value_(value) {}

std::uint64_t RoomId::value() const noexcept { return value_; }

BattleInstanceId::BattleInstanceId(std::uint64_t value) noexcept
    : value_(value) {}

std::uint64_t BattleInstanceId::value() const noexcept { return value_; }

} // namespace lol::shared
