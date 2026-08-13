#include <lol/battle/LootApi.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace lol::battle {

namespace {

// Fixed private deterministic PRNG. floating point, <random> engine,
// std::*_distribution, std::hash, reinterpret_cast, host endian은 사용하지
// 않는다. unsigned 64-bit overflow는 modulo 2^64을 의도한다.

constexpr std::uint64_t kFnvOffsetBasis = UINT64_C(0xcbf29ce484222325);
constexpr std::uint64_t kFnvPrime = UINT64_C(0x00000100000001b3);
constexpr std::uint64_t kSplitMixGamma = UINT64_C(0x9e3779b97f4a7c15);

std::uint64_t fnvAppendBytes(std::uint64_t state, std::uint64_t value,
                             std::uint32_t byteCount) noexcept {
  for (std::uint32_t byte = 0; byte < byteCount; ++byte) {
    const auto current =
        static_cast<std::uint8_t>((value >> (byte * 8)) & 0xff);
    state ^= current;
    state *= kFnvPrime;
  }
  return state;
}

std::uint64_t seedFor(shared::RoomId roomId, shared::BattleInstanceId battleId,
                      std::uint16_t rulesetVersion) noexcept {
  std::uint64_t state = kFnvOffsetBasis;
  state = fnvAppendBytes(state, roomId.value(), 8);
  state = fnvAppendBytes(state, battleId.value(), 8);
  state = fnvAppendBytes(state, rulesetVersion, 2);
  return state;
}

struct SplitMix64 final {
  std::uint64_t state;

  std::uint64_t next() noexcept {
    state += kSplitMixGamma;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
  }
};

std::int32_t clampedCoordinate(std::uint64_t random) noexcept {
  constexpr std::uint64_t kSpan = 20001;
  const auto offset = random % kSpan;
  const auto candidate =
      -RelicRuleset::arenaBoundMillimeters + static_cast<std::int32_t>(offset);
  if (candidate < -RelicRuleset::arenaBoundMillimeters) {
    return -RelicRuleset::arenaBoundMillimeters;
  }
  if (candidate > RelicRuleset::arenaBoundMillimeters) {
    return RelicRuleset::arenaBoundMillimeters;
  }
  return candidate;
}

DropPosition nextPosition(SplitMix64 &prng) noexcept {
  return DropPosition{clampedCoordinate(prng.next()),
                      clampedCoordinate(prng.next())};
}

} // namespace

std::optional<std::vector<RelicDrop>>
generateDrops(shared::RoomId roomId, shared::BattleInstanceId battleId,
              std::uint16_t rulesetVersion, std::uint32_t capturedCount,
              const RelicCatalog &catalog) {
  if (capturedCount < RelicRuleset::minCapturedParticipants ||
      capturedCount > RelicRuleset::maxCapturedParticipants) {
    return std::nullopt;
  }
  // 생성할 item이 snapshot에 있는지 확인한다. snapshot은 immutable이므로
  // v1 정상 경로에서는 항상 충족되고, 부재 시 거짓 drop을 만들지 않는다.
  if (!catalog.unitValueOf(RelicRuleset::commonItemId).has_value() ||
      !catalog.unitValueOf(RelicRuleset::rareItemId).has_value()) {
    return std::nullopt;
  }

  std::vector<RelicDrop> drops;
  drops.reserve(capturedCount);
  SplitMix64 prng{seedFor(roomId, battleId, rulesetVersion)};
  for (std::uint32_t index = 0; index < capturedCount; ++index) {
    const auto itemId =
        index == 0 ? RelicRuleset::rareItemId : RelicRuleset::commonItemId;
    drops.push_back(
        RelicDrop{DropId{index + 1}, itemId, 1, nextPosition(prng)});
  }
  return drops;
}

} // namespace lol::battle
