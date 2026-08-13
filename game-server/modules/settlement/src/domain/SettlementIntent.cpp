#include <lol/settlement/SettlementIntent.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lol::settlement {
namespace {

// Minimal SHA-256 (FIPS 180-4) kept private to this translation unit. It is
// used only to derive SettlementId, SettlementBatchId, and the canonical
// payload hash. No other code depends on it.

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::array<std::uint32_t, 8> kSha256InitialHash = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

constexpr std::uint32_t rotateRight(std::uint32_t value, unsigned shift) {
  return (value >> shift) | (value << (32u - shift));
}

std::array<std::uint8_t, 32> sha256(const std::vector<std::uint8_t> &input) {
  const auto bitLength = static_cast<std::uint64_t>(input.size()) * 8u;
  std::vector<std::uint8_t> padded = input;
  padded.push_back(static_cast<std::uint8_t>(0x80u));
  while ((padded.size() % 64u) != 56u) {
    padded.push_back(static_cast<std::uint8_t>(0u));
  }
  for (std::int32_t shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(
        static_cast<std::uint8_t>(bitLength >> static_cast<unsigned>(shift)));
  }

  auto h = kSha256InitialHash;
  for (std::size_t offset = 0; offset < padded.size(); offset += 64u) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t index = 0; index < 16; ++index) {
      const auto base = offset + index * 4u;
      w[index] = (static_cast<std::uint32_t>(padded[base]) << 24u) |
                 (static_cast<std::uint32_t>(padded[base + 1u]) << 16u) |
                 (static_cast<std::uint32_t>(padded[base + 2u]) << 8u) |
                 static_cast<std::uint32_t>(padded[base + 3u]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
      const auto sigma0 = rotateRight(w[index - 15u], 7u) ^
                          rotateRight(w[index - 15u], 18u) ^
                          (w[index - 15u] >> 3u);
      const auto sigma1 = rotateRight(w[index - 2u], 17u) ^
                          rotateRight(w[index - 2u], 19u) ^
                          (w[index - 2u] >> 10u);
      w[index] = w[index - 16u] + sigma0 + w[index - 7u] + sigma1;
    }

    auto a = h[0];
    auto b = h[1];
    auto c = h[2];
    auto d = h[3];
    auto e = h[4];
    auto f = h[5];
    auto g = h[6];
    auto hh = h[7];
    for (std::size_t index = 0; index < 64; ++index) {
      const auto bigSigma1 =
          rotateRight(e, 6u) ^ rotateRight(e, 11u) ^ rotateRight(e, 25u);
      const auto ch = (e & f) ^ ((~e) & g);
      const auto temp1 =
          hh + bigSigma1 + ch + kSha256RoundConstants[index] + w[index];
      const auto bigSigma0 =
          rotateRight(a, 2u) ^ rotateRight(a, 13u) ^ rotateRight(a, 22u);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = bigSigma0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    digest[index * 4u] = static_cast<std::uint8_t>(h[index] >> 24u);
    digest[index * 4u + 1u] = static_cast<std::uint8_t>(h[index] >> 16u);
    digest[index * 4u + 2u] = static_cast<std::uint8_t>(h[index] >> 8u);
    digest[index * 4u + 3u] = static_cast<std::uint8_t>(h[index]);
  }
  return digest;
}

void appendU64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (std::int32_t shift = 56; shift >= 0; shift -= 8) {
    out.push_back(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
  }
}

void appendU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8u));
  out.push_back(static_cast<std::uint8_t>(value));
}

void appendAscii(std::vector<std::uint8_t> &out, const char *text) {
  while (*text != '\0') {
    out.push_back(static_cast<std::uint8_t>(*text));
    ++text;
  }
}

SettlementId computeSettlementId(shared::RoomId roomId,
                                 shared::BattleInstanceId battleId,
                                 const shared::AccountId &accountId) {
  std::vector<std::uint8_t> input;
  input.reserve(15u + 8u + 8u + 16u);
  appendAscii(input, "settlement-v1");
  appendU64(input, roomId.value());
  appendU64(input, battleId.value());
  const auto &bytes = accountId.bytes();
  input.insert(input.end(), bytes.begin(), bytes.end());
  const auto digest = sha256(input);
  SettlementId::Bytes idBytes{};
  std::copy(digest.begin(), digest.begin() + 16, idBytes.begin());
  return SettlementId{idBytes};
}

SettlementBatchId computeSettlementBatchId(shared::RoomId roomId,
                                           shared::BattleInstanceId battleId) {
  std::vector<std::uint8_t> input;
  input.reserve(18u + 8u + 8u);
  appendAscii(input, "settlement-batch-v1");
  appendU64(input, roomId.value());
  appendU64(input, battleId.value());
  const auto digest = sha256(input);
  SettlementBatchId::Bytes idBytes{};
  std::copy(digest.begin(), digest.begin() + 16, idBytes.begin());
  return SettlementBatchId{idBytes};
}

std::vector<std::uint8_t> assemblePayload(
    const SettlementId &id, const shared::AccountId &accountId,
    shared::RoomId roomId, shared::BattleInstanceId battleId,
    BattleOutcome outcome, ParticipantExitStatus exitStatus,
    std::uint16_t catalogVersion, const std::vector<ItemDelta> &deltas,
    std::uint64_t finalAssetValue, const ResultCommittedAt &committedAt) {
  std::vector<std::uint8_t> out;
  out.reserve(20u + 16u + 16u + 8u + 8u + 1u + 1u + 2u + 2u +
              deltas.size() * 16u + 8u + 8u + 8u);
  appendAscii(out, "settlement-intent-v1");
  const auto &idBytes = id.bytes();
  out.insert(out.end(), idBytes.begin(), idBytes.end());
  const auto &accountBytes = accountId.bytes();
  out.insert(out.end(), accountBytes.begin(), accountBytes.end());
  appendU64(out, roomId.value());
  appendU64(out, battleId.value());
  out.push_back(static_cast<std::uint8_t>(outcome));
  out.push_back(static_cast<std::uint8_t>(exitStatus));
  appendU16(out, catalogVersion);
  appendU16(out, static_cast<std::uint16_t>(deltas.size()));
  for (const auto &delta : deltas) {
    appendU64(out, delta.itemId);
    appendU64(out, delta.quantity);
  }
  appendU64(out, finalAssetValue);
  appendU64(out, committedAt.unixEpochMilliseconds);
  appendU64(out, committedAt.monotonicNanoseconds);
  return out;
}

} // namespace

SettlementId::SettlementId(Bytes bytes) noexcept : bytes_(bytes) {}
const SettlementId::Bytes &SettlementId::bytes() const noexcept {
  return bytes_;
}

SettlementBatchId::SettlementBatchId(Bytes bytes) noexcept : bytes_(bytes) {}
const SettlementBatchId::Bytes &SettlementBatchId::bytes() const noexcept {
  return bytes_;
}

CanonicalPayloadHash::CanonicalPayloadHash(Bytes bytes) noexcept
    : bytes_(bytes) {}
const CanonicalPayloadHash::Bytes &
CanonicalPayloadHash::bytes() const noexcept {
  return bytes_;
}

SettlementIntent::SettlementIntent(
    shared::AccountId accountId, ParticipantExitStatus exitStatus,
    std::vector<ItemDelta> itemDeltas, std::uint64_t finalAssetValue,
    shared::RoomId roomId, shared::BattleInstanceId battleId,
    BattleOutcome outcome, std::uint16_t catalogVersion,
    ResultCommittedAt committedAt, SettlementId id,
    CanonicalPayloadHash canonicalHash) noexcept
    : accountId_(std::move(accountId)), exitStatus_(exitStatus),
      itemDeltas_(std::move(itemDeltas)), finalAssetValue_(finalAssetValue),
      roomId_(roomId), battleId_(battleId), outcome_(outcome),
      catalogVersion_(catalogVersion), committedAt_(committedAt),
      id_(std::move(id)), canonicalHash_(std::move(canonicalHash)) {}

const shared::AccountId &SettlementIntent::accountId() const noexcept {
  return accountId_;
}

ParticipantExitStatus SettlementIntent::exitStatus() const noexcept {
  return exitStatus_;
}

const std::vector<ItemDelta> &SettlementIntent::itemDeltas() const noexcept {
  return itemDeltas_;
}

std::uint64_t SettlementIntent::finalAssetValue() const noexcept {
  return finalAssetValue_;
}

const shared::RoomId &SettlementIntent::roomId() const noexcept {
  return roomId_;
}

const shared::BattleInstanceId &SettlementIntent::battleId() const noexcept {
  return battleId_;
}

BattleOutcome SettlementIntent::outcome() const noexcept { return outcome_; }

std::uint16_t SettlementIntent::catalogVersion() const noexcept {
  return catalogVersion_;
}

const ResultCommittedAt &SettlementIntent::committedAt() const noexcept {
  return committedAt_;
}

const SettlementId &SettlementIntent::id() const noexcept { return id_; }

const CanonicalPayloadHash &SettlementIntent::canonicalHash() const noexcept {
  return canonicalHash_;
}

SettlementIntentBatch::SettlementIntentBatch(
    SettlementBatchId id, shared::RoomId roomId,
    shared::BattleInstanceId battleId, BattleOutcome outcome,
    std::uint16_t catalogVersion, ResultCommittedAt committedAt,
    std::vector<SettlementIntent> intents) noexcept
    : id_(std::move(id)), roomId_(roomId), battleId_(battleId),
      outcome_(outcome), catalogVersion_(catalogVersion),
      committedAt_(committedAt), intents_(std::move(intents)) {}

const SettlementBatchId &SettlementIntentBatch::id() const noexcept {
  return id_;
}

const shared::RoomId &SettlementIntentBatch::roomId() const noexcept {
  return roomId_;
}

const shared::BattleInstanceId &
SettlementIntentBatch::battleId() const noexcept {
  return battleId_;
}

BattleOutcome SettlementIntentBatch::outcome() const noexcept {
  return outcome_;
}

std::uint16_t SettlementIntentBatch::catalogVersion() const noexcept {
  return catalogVersion_;
}

const ResultCommittedAt &SettlementIntentBatch::committedAt() const noexcept {
  return committedAt_;
}

const std::vector<SettlementIntent> &
SettlementIntentBatch::intents() const noexcept {
  return intents_;
}

std::optional<SettlementIntentBatch>
createSettlementIntentBatch(const SettlementIntentBatchSource &source) {
  const auto outcome = source.outcome;
  const bool outcomeValid =
      outcome == BattleOutcome::MonsterDefeated ||
      outcome == BattleOutcome::CombatTimeout ||
      outcome == BattleOutcome::CancelledNoActiveParticipants;
  if (source.roomId.value() == 0 || source.battleId.value() == 0 ||
      source.catalogVersion == 0 || !outcomeValid ||
      source.participants.size() < 2 || source.participants.size() > 10) {
    return std::nullopt;
  }

  // Normalized participants: unique AccountIds in raw byte ascending order.
  auto participants = source.participants;
  std::sort(participants.begin(), participants.end(),
            [](const SettlementParticipantSource &lhs,
               const SettlementParticipantSource &rhs) {
              return lhs.accountId < rhs.accountId;
            });
  for (std::size_t index = 1; index < participants.size(); ++index) {
    if (participants[index - 1u].accountId == participants[index].accountId) {
      return std::nullopt;
    }
  }

  // Normalized item deltas: item count fits u16, nonzero unique itemIds,
  // positive quantities, sorted by itemId. CombatTimeout has no items and
  // value zero.
  for (auto &participant : participants) {
    // Exit status is part of the frozen canonical payload; only the two
    // accepted values may ever reach ID/payload/hash construction.
    if (participant.exitStatus != ParticipantExitStatus::TerminalPresent &&
        participant.exitStatus != ParticipantExitStatus::TerminalExited) {
      return std::nullopt;
    }
    if (outcome == BattleOutcome::CancelledNoActiveParticipants &&
        participant.exitStatus != ParticipantExitStatus::TerminalExited) {
      return std::nullopt;
    }
    if (participant.itemDeltas.size() >
        std::numeric_limits<std::uint16_t>::max()) {
      return std::nullopt;
    }
    std::sort(participant.itemDeltas.begin(), participant.itemDeltas.end(),
              [](const ItemDelta &lhs, const ItemDelta &rhs) {
                return lhs.itemId < rhs.itemId;
              });
    for (const auto &delta : participant.itemDeltas) {
      if (delta.itemId == 0 || delta.quantity == 0) {
        return std::nullopt;
      }
    }
    for (std::size_t index = 1; index < participant.itemDeltas.size();
         ++index) {
      if (participant.itemDeltas[index - 1u].itemId ==
          participant.itemDeltas[index].itemId) {
        return std::nullopt;
      }
    }
    if (outcome == BattleOutcome::CombatTimeout &&
        (!participant.itemDeltas.empty() || participant.finalAssetValue != 0)) {
      return std::nullopt;
    }
  }

  const auto batchId = computeSettlementBatchId(source.roomId, source.battleId);
  std::vector<SettlementIntent> intents;
  intents.reserve(participants.size());
  for (const auto &participant : participants) {
    const auto id = computeSettlementId(source.roomId, source.battleId,
                                        participant.accountId);
    const auto payload = assemblePayload(
        id, participant.accountId, source.roomId, source.battleId, outcome,
        participant.exitStatus, source.catalogVersion, participant.itemDeltas,
        participant.finalAssetValue, source.committedAt);
    const auto digest = sha256(payload);
    CanonicalPayloadHash::Bytes hashBytes{};
    std::copy(digest.begin(), digest.end(), hashBytes.begin());
    intents.push_back(SettlementIntent{
        participant.accountId, participant.exitStatus, participant.itemDeltas,
        participant.finalAssetValue, source.roomId, source.battleId, outcome,
        source.catalogVersion, source.committedAt, id,
        CanonicalPayloadHash{hashBytes}});
  }
  return SettlementIntentBatch{
      batchId,           source.roomId,         source.battleId,
      outcome,           source.catalogVersion, source.committedAt,
      std::move(intents)};
}

std::vector<std::uint8_t> canonicalPayload(const SettlementIntent &intent) {
  return assemblePayload(
      intent.id(), intent.accountId(), intent.roomId(), intent.battleId(),
      intent.outcome(), intent.exitStatus(), intent.catalogVersion(),
      intent.itemDeltas(), intent.finalAssetValue(), intent.committedAt());
}

CanonicalPayloadHash
hashCanonicalPayload(std::span<const std::uint8_t> payload) {
  return CanonicalPayloadHash{
      sha256(std::vector<std::uint8_t>{payload.begin(), payload.end()})};
}

} // namespace lol::settlement
