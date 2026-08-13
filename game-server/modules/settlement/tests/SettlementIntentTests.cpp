#include <lol/settlement/SettlementIntent.hpp>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using lol::settlement::BattleOutcome;
using lol::settlement::canonicalPayload;
using lol::settlement::CanonicalPayloadHash;
using lol::settlement::createSettlementIntentBatch;
using lol::settlement::ItemDelta;
using lol::settlement::ParticipantExitStatus;
using lol::settlement::ResultCommittedAt;
using lol::settlement::SettlementBatchId;
using lol::settlement::SettlementId;
using lol::settlement::SettlementIntentBatch;
using lol::settlement::SettlementIntentBatchSource;
using lol::settlement::SettlementParticipantSource;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;

constexpr std::uint64_t kMonsterDefeatedWall = UINT64_C(1760000000123);
constexpr std::uint64_t kMonsterDefeatedMono = UINT64_C(9876543210);
constexpr std::uint64_t kCombatTimeoutWall = UINT64_C(1760000001123);
constexpr std::uint64_t kCombatTimeoutMono = UINT64_C(9876544210);
constexpr std::uint64_t kCancelledNoHoldingsWall = UINT64_C(1760000002123);
constexpr std::uint64_t kCancelledNoHoldingsMono = UINT64_C(9876545210);
constexpr std::uint64_t kCancelledPartialHoldingsWall = UINT64_C(1760000003123);
constexpr std::uint64_t kCancelledPartialHoldingsMono = UINT64_C(9876546210);

static_assert(static_cast<std::uint8_t>(BattleOutcome::MonsterDefeated) == 1);
static_assert(static_cast<std::uint8_t>(BattleOutcome::CombatTimeout) == 2);
static_assert(static_cast<std::uint8_t>(
                  BattleOutcome::CancelledNoActiveParticipants) == 3);

const AccountId kAccountA =
    AccountId{AccountId::Bytes{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                               0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f}};
const AccountId kAccountB =
    AccountId{AccountId::Bytes{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                               0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f}};

AccountId accountSuffix(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

SettlementId::Bytes bytes16FromHex(const std::string &hex) {
  SettlementId::Bytes bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(
        std::stoul(hex.substr(index * 2, 2), nullptr, 16));
  }
  return bytes;
}

CanonicalPayloadHash::Bytes bytes32FromHex(const std::string &hex) {
  CanonicalPayloadHash::Bytes bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(
        std::stoul(hex.substr(index * 2, 2), nullptr, 16));
  }
  return bytes;
}

std::vector<std::uint8_t> payloadFromHex(const std::string &hex) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    bytes.push_back(static_cast<std::uint8_t>(
        std::stoul(hex.substr(index, 2), nullptr, 16)));
  }
  return bytes;
}

bool expectBytes(const std::vector<std::uint8_t> &actual,
                 const std::string &expectedHex) {
  return actual == payloadFromHex(expectedHex);
}

SettlementParticipantSource participantSource(AccountId accountId,
                                              ParticipantExitStatus exitStatus,
                                              std::vector<ItemDelta> itemDeltas,
                                              std::uint64_t value) {
  return SettlementParticipantSource{
      .accountId = std::move(accountId),
      .exitStatus = exitStatus,
      .itemDeltas = std::move(itemDeltas),
      .finalAssetValue = value,
  };
}

SettlementIntentBatchSource
monsterDefeatedSource(std::vector<SettlementParticipantSource> participants) {
  return SettlementIntentBatchSource{
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{11},
      .outcome = BattleOutcome::MonsterDefeated,
      .catalogVersion = 1,
      .committedAt =
          ResultCommittedAt{kMonsterDefeatedWall, kMonsterDefeatedMono},
      .participants = std::move(participants),
  };
}

SettlementIntentBatchSource
combatTimeoutSource(std::vector<SettlementParticipantSource> participants) {
  return SettlementIntentBatchSource{
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{12},
      .outcome = BattleOutcome::CombatTimeout,
      .catalogVersion = 1,
      .committedAt = ResultCommittedAt{kCombatTimeoutWall, kCombatTimeoutMono},
      .participants = std::move(participants),
  };
}

SettlementIntentBatchSource
cancelledSource(BattleInstanceId battleId, ResultCommittedAt committedAt,
                std::vector<SettlementParticipantSource> participants) {
  return SettlementIntentBatchSource{
      .roomId = RoomId{7},
      .battleId = battleId,
      .outcome = BattleOutcome::CancelledNoActiveParticipants,
      .catalogVersion = 1,
      .committedAt = committedAt,
      .participants = std::move(participants),
  };
}

bool rejects(SettlementIntentBatchSource source) {
  return !createSettlementIntentBatch(source).has_value();
}

// Golden MonsterDefeated batch: room 7, battle 11, catalog 1, wall
// 1760000000123, monotonic 9876543210, batch id
// 620b39c0db2793374d8a5ef7e3441536. A TerminalPresent item 2 qty1 value300, B
// TerminalExited item 1 qty1 value100.
bool goldenMonsterDefeatedBatch() {
  auto batch = createSettlementIntentBatch(monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{2, 1}}, 300),
       participantSource(kAccountB, ParticipantExitStatus::TerminalExited,
                         {{1, 1}}, 100)}));
  if (!batch.has_value() ||
      batch->id() != SettlementBatchId{bytes16FromHex(
                         "620b39c0db2793374d8a5ef7e3441536")} ||
      batch->roomId() != RoomId{7} ||
      batch->battleId() != BattleInstanceId{11} ||
      batch->outcome() != BattleOutcome::MonsterDefeated ||
      batch->catalogVersion() != 1 ||
      batch->committedAt() !=
          ResultCommittedAt{kMonsterDefeatedWall, kMonsterDefeatedMono} ||
      batch->intents().size() != 2) {
    return false;
  }
  const auto &a = batch->intents()[0];
  const auto &b = batch->intents()[1];
  if (a.accountId() != kAccountA ||
      a.exitStatus() != ParticipantExitStatus::TerminalPresent ||
      a.itemDeltas() != std::vector<ItemDelta>{{2, 1}} ||
      a.finalAssetValue() != 300 || a.roomId() != RoomId{7} ||
      a.battleId() != BattleInstanceId{11} ||
      a.outcome() != BattleOutcome::MonsterDefeated ||
      a.catalogVersion() != 1 ||
      a.committedAt() !=
          ResultCommittedAt{kMonsterDefeatedWall, kMonsterDefeatedMono} ||
      a.id() !=
          SettlementId{bytes16FromHex("d78f45fdc179931d82a4d1c390e84eae")} ||
      a.canonicalHash() != CanonicalPayloadHash{bytes32FromHex(
                               "e29758929c2b28eeb609f9f3f5f321de183e9f0acd0ff91"
                               "ebe11d13e9ef51fd9")} ||
      !expectBytes(canonicalPayload(a), "736574746c656d656e742d696e74656e742d76"
                                        "31d78f45fdc179931d82a4d1c390e84eae"
                                        "000102030405060708090a0b0c0d0e0f"
                                        "0000000000000007"
                                        "000000000000000b"
                                        "0101"
                                        "0001"
                                        "0001"
                                        "0000000000000002"
                                        "0000000000000001"
                                        "000000000000012c"
                                        "00000199c82cc07b"
                                        "000000024cb016ea")) {
    return false;
  }
  if (b.accountId() != kAccountB ||
      b.exitStatus() != ParticipantExitStatus::TerminalExited ||
      b.itemDeltas() != std::vector<ItemDelta>{{1, 1}} ||
      b.finalAssetValue() != 100 || b.roomId() != RoomId{7} ||
      b.battleId() != BattleInstanceId{11} ||
      b.outcome() != BattleOutcome::MonsterDefeated ||
      b.catalogVersion() != 1 ||
      b.committedAt() !=
          ResultCommittedAt{kMonsterDefeatedWall, kMonsterDefeatedMono} ||
      b.id() !=
          SettlementId{bytes16FromHex("9428b0d4de2bda88d25124440f4e575d")} ||
      b.canonicalHash() != CanonicalPayloadHash{bytes32FromHex(
                               "f979114411a3e75e428df7e508d8e500efa9e7ce13e3fc7"
                               "33978493d61c7c321")} ||
      !expectBytes(canonicalPayload(b), "736574746c656d656e742d696e74656e742d76"
                                        "319428b0d4de2bda88d25124440f4e575d"
                                        "101112131415161718191a1b1c1d1e1f"
                                        "0000000000000007"
                                        "000000000000000b"
                                        "0102"
                                        "0001"
                                        "0001"
                                        "0000000000000001"
                                        "0000000000000001"
                                        "0000000000000064"
                                        "00000199c82cc07b"
                                        "000000024cb016ea")) {
    return false;
  }
  return true;
}

// Golden CombatTimeout batch: room 7, battle 12, catalog 1, wall
// 1760000001123, monotonic 9876544210, batch id
// a9487234b9dc1783019d7c1617db461c. Same A/B accounts, no items, value zero.
bool goldenCombatTimeoutBatch() {
  auto batch = createSettlementIntentBatch(combatTimeoutSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent, {},
                         0),
       participantSource(kAccountB, ParticipantExitStatus::TerminalExited, {},
                         0)}));
  if (!batch.has_value() ||
      batch->id() != SettlementBatchId{bytes16FromHex(
                         "a9487234b9dc1783019d7c1617db461c")} ||
      batch->roomId() != RoomId{7} ||
      batch->battleId() != BattleInstanceId{12} ||
      batch->outcome() != BattleOutcome::CombatTimeout ||
      batch->catalogVersion() != 1 ||
      batch->committedAt() !=
          ResultCommittedAt{kCombatTimeoutWall, kCombatTimeoutMono} ||
      batch->intents().size() != 2) {
    return false;
  }
  const auto &a = batch->intents()[0];
  const auto &b = batch->intents()[1];
  if (a.accountId() != kAccountA ||
      a.exitStatus() != ParticipantExitStatus::TerminalPresent ||
      !a.itemDeltas().empty() || a.finalAssetValue() != 0 ||
      a.id() !=
          SettlementId{bytes16FromHex("72d371240fe83e824850c102c181c640")} ||
      a.canonicalHash() != CanonicalPayloadHash{bytes32FromHex(
                               "2193598d897d76aa8936893e220e8c15c274e506e492898"
                               "9c94fb253af386af8")} ||
      !expectBytes(canonicalPayload(a), "736574746c656d656e742d696e74656e742d76"
                                        "3172d371240fe83e824850c102c181c640"
                                        "000102030405060708090a0b0c0d0e0f"
                                        "0000000000000007"
                                        "000000000000000c"
                                        "0201"
                                        "0001"
                                        "0000"
                                        "0000000000000000"
                                        "00000199c82cc463"
                                        "000000024cb01ad2")) {
    return false;
  }
  if (b.accountId() != kAccountB ||
      b.exitStatus() != ParticipantExitStatus::TerminalExited ||
      !b.itemDeltas().empty() || b.finalAssetValue() != 0 ||
      b.id() !=
          SettlementId{bytes16FromHex("716296a90f6d821ffde72e52d63cc326")} ||
      b.canonicalHash() != CanonicalPayloadHash{bytes32FromHex(
                               "cccb88ac136625ef1dde3371814926c363d24c23041fc1b"
                               "9839a0b0f51196c27")} ||
      !expectBytes(canonicalPayload(b), "736574746c656d656e742d696e74656e742d76"
                                        "31716296a90f6d821ffde72e52d63cc326"
                                        "101112131415161718191a1b1c1d1e1f"
                                        "0000000000000007"
                                        "000000000000000c"
                                        "0202"
                                        "0001"
                                        "0000"
                                        "0000000000000000"
                                        "00000199c82cc463"
                                        "000000024cb01ad2")) {
    return false;
  }
  return true;
}

bool goldenCancelledNoHoldingsBatch() {
  auto batch = createSettlementIntentBatch(cancelledSource(
      BattleInstanceId{13},
      ResultCommittedAt{kCancelledNoHoldingsWall, kCancelledNoHoldingsMono},
      {participantSource(kAccountA, ParticipantExitStatus::TerminalExited, {},
                         0),
       participantSource(kAccountB, ParticipantExitStatus::TerminalExited, {},
                         0)}));
  if (!batch.has_value() ||
      batch->id() != SettlementBatchId{bytes16FromHex(
                         "3ff37077fe0c48a3ba3ef9b8fa9a8873")} ||
      batch->outcome() != BattleOutcome::CancelledNoActiveParticipants ||
      batch->intents().size() != 2) {
    return false;
  }
  const auto &a = batch->intents()[0];
  const auto &b = batch->intents()[1];
  if (a.exitStatus() != ParticipantExitStatus::TerminalExited ||
      !a.itemDeltas().empty() || a.finalAssetValue() != 0 ||
      a.id() !=
          SettlementId{bytes16FromHex("eb938ba20377c4d08499ae6711acdd3f")} ||
      a.canonicalHash() != CanonicalPayloadHash{bytes32FromHex(
                               "884d15b88a413c138053e3571acd879ceb950c52d03a121"
                               "0d55c64ef8dab8c74")} ||
      !expectBytes(canonicalPayload(a), "736574746c656d656e742d696e74656e742d76"
                                        "31eb938ba20377c4d08499ae6711acdd3f"
                                        "000102030405060708090a0b0c0d0e0f"
                                        "0000000000000007"
                                        "000000000000000d"
                                        "0302"
                                        "0001"
                                        "0000"
                                        "0000000000000000"
                                        "00000199c82cc84b"
                                        "000000024cb01eba")) {
    return false;
  }
  return b.exitStatus() == ParticipantExitStatus::TerminalExited &&
         b.itemDeltas().empty() && b.finalAssetValue() == 0 &&
         b.id() ==
             SettlementId{bytes16FromHex("3195536bd39f4cb7937a127be77333b1")} &&
         b.canonicalHash() ==
             CanonicalPayloadHash{bytes32FromHex(
                 "34e6e943454b98eafd718ed10cec845d1e0cc194fea8e50"
                 "79f581179c9917a98")} &&
         expectBytes(canonicalPayload(b),
                     "736574746c656d656e742d696e74656e742d76"
                     "313195536bd39f4cb7937a127be77333b1"
                     "101112131415161718191a1b1c1d1e1f"
                     "0000000000000007"
                     "000000000000000d"
                     "0302"
                     "0001"
                     "0000"
                     "0000000000000000"
                     "00000199c82cc84b"
                     "000000024cb01eba");
}

bool goldenCancelledPartialHoldingsBatch() {
  auto batch = createSettlementIntentBatch(cancelledSource(
      BattleInstanceId{14},
      ResultCommittedAt{kCancelledPartialHoldingsWall,
                        kCancelledPartialHoldingsMono},
      {participantSource(kAccountA, ParticipantExitStatus::TerminalExited,
                         {{2, 1}}, 300),
       participantSource(kAccountB, ParticipantExitStatus::TerminalExited, {},
                         0)}));
  if (!batch.has_value() ||
      batch->id() != SettlementBatchId{bytes16FromHex(
                         "eb1c7fcc0af24bdd54380b8b9b0c3f2f")} ||
      batch->outcome() != BattleOutcome::CancelledNoActiveParticipants ||
      batch->intents().size() != 2) {
    return false;
  }
  const auto &a = batch->intents()[0];
  const auto &b = batch->intents()[1];
  if (a.exitStatus() != ParticipantExitStatus::TerminalExited ||
      a.itemDeltas() != std::vector<ItemDelta>{{2, 1}} ||
      a.finalAssetValue() != 300 ||
      a.id() !=
          SettlementId{bytes16FromHex("43eca108368cdc1278fea78b755851d9")} ||
      a.canonicalHash() != CanonicalPayloadHash{bytes32FromHex(
                               "ec7e01d56a468e0d140eea4cff20d1f65be2be5b50ed10"
                               "f629512157c883201a")} ||
      !expectBytes(canonicalPayload(a), "736574746c656d656e742d696e74656e742d76"
                                        "3143eca108368cdc1278fea78b755851d9"
                                        "000102030405060708090a0b0c0d0e0f"
                                        "0000000000000007"
                                        "000000000000000e"
                                        "0302"
                                        "0001"
                                        "0001"
                                        "0000000000000002"
                                        "0000000000000001"
                                        "000000000000012c"
                                        "00000199c82ccc33"
                                        "000000024cb022a2")) {
    return false;
  }
  return b.exitStatus() == ParticipantExitStatus::TerminalExited &&
         b.itemDeltas().empty() && b.finalAssetValue() == 0 &&
         b.id() ==
             SettlementId{bytes16FromHex("65391736861a52e68a81c8c53b72e815")} &&
         b.canonicalHash() ==
             CanonicalPayloadHash{bytes32FromHex(
                 "d0263bd506ad8066ec040d15e0737d7ee121de5acac9e96"
                 "0c1bb054e9434f213")} &&
         expectBytes(canonicalPayload(b),
                     "736574746c656d656e742d696e74656e742d76"
                     "3165391736861a52e68a81c8c53b72e815"
                     "101112131415161718191a1b1c1d1e1f"
                     "0000000000000007"
                     "000000000000000e"
                     "0302"
                     "0001"
                     "0000"
                     "0000000000000000"
                     "00000199c82ccc33"
                     "000000024cb022a2");
}

bool cancellationRequiresEveryParticipantExited() {
  auto valid = cancelledSource(
      BattleInstanceId{13},
      ResultCommittedAt{kCancelledNoHoldingsWall, kCancelledNoHoldingsMono},
      {participantSource(kAccountA, ParticipantExitStatus::TerminalExited,
                         {{2, 1}}, 300),
       participantSource(kAccountB, ParticipantExitStatus::TerminalExited, {},
                         0)});
  if (!createSettlementIntentBatch(valid).has_value()) {
    return false;
  }
  valid.participants[0].exitStatus = ParticipantExitStatus::TerminalPresent;
  return rejects(std::move(valid));
}

// The same semantic source presented in different participant and item delta
// orders produces an exactly equal batch: intents sorted by AccountId raw
// bytes, item deltas sorted by itemId.
bool permutationsNormalizeIdentically() {
  auto canonical = createSettlementIntentBatch(monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{2, 1}, {1, 1}}, 400),
       participantSource(kAccountB, ParticipantExitStatus::TerminalExited,
                         {{4, 3}, {3, 2}}, 500)}));
  auto permuted = createSettlementIntentBatch(monsterDefeatedSource(
      {participantSource(kAccountB, ParticipantExitStatus::TerminalExited,
                         {{3, 2}, {4, 3}}, 500),
       participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{1, 1}, {2, 1}}, 400)}));
  if (!canonical.has_value() || !permuted.has_value() ||
      canonical != permuted || canonical->intents().size() != 2) {
    return false;
  }
  const auto &a = canonical->intents()[0];
  const auto &b = canonical->intents()[1];
  return a.accountId() == kAccountA &&
         a.itemDeltas() == std::vector<ItemDelta>{{1, 1}, {2, 1}} &&
         b.accountId() == kAccountB &&
         b.itemDeltas() == std::vector<ItemDelta>{{3, 2}, {4, 3}};
}

// Exactly one intent is produced per participant source and the source exit
// status is preserved on the intent.
bool oneIntentPerSourceAndExitStatusPreserved() {
  const AccountId c = AccountId{
      AccountId::Bytes{0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                       0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f}};
  auto batch = createSettlementIntentBatch(monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent, {},
                         0),
       participantSource(c, ParticipantExitStatus::TerminalExited, {{9, 1}},
                         10),
       participantSource(kAccountB, ParticipantExitStatus::TerminalPresent,
                         {{7, 2}}, 20)}));
  if (!batch.has_value() || batch->intents().size() != 3) {
    return false;
  }
  const auto &intents = batch->intents();
  return intents[0].accountId() == kAccountA &&
         intents[0].exitStatus() == ParticipantExitStatus::TerminalPresent &&
         intents[1].accountId() == kAccountB &&
         intents[1].exitStatus() == ParticipantExitStatus::TerminalPresent &&
         intents[2].accountId() == c &&
         intents[2].exitStatus() == ParticipantExitStatus::TerminalExited;
}

// The SettlementId depends only on room, battle, and account identity: a
// changed payload keeps the same id but changes the canonical hash.
bool sameIdentityKeepsIdAndChangesHash() {
  auto first = createSettlementIntentBatch(monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{2, 1}}, 300),
       participantSource(kAccountB, ParticipantExitStatus::TerminalExited,
                         {{1, 1}}, 100)}));
  auto second = createSettlementIntentBatch(monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{2, 2}}, 600),
       participantSource(kAccountB, ParticipantExitStatus::TerminalExited,
                         {{1, 1}}, 100)}));
  if (!first.has_value() || !second.has_value() ||
      first->intents().size() != 2 || second->intents().size() != 2) {
    return false;
  }
  const auto &firstA = first->intents()[0];
  const auto &secondA = second->intents()[0];
  return firstA.id() == secondA.id() &&
         firstA.canonicalHash() != secondA.canonicalHash() &&
         canonicalPayload(firstA) != canonicalPayload(secondA) &&
         firstA.itemDeltas() != secondA.itemDeltas();
}

// Every invalid source is rejected without producing a batch.
bool invalidSourcesReturnNoBatch() {
  const auto aPresent = participantSource(
      kAccountA, ParticipantExitStatus::TerminalPresent, {{2, 1}}, 300);
  const auto bExited = participantSource(
      kAccountB, ParticipantExitStatus::TerminalExited, {{1, 1}}, 100);

  auto zeroRoom = monsterDefeatedSource({aPresent, bExited});
  zeroRoom.roomId = RoomId{0};
  if (!rejects(zeroRoom)) {
    return false;
  }

  auto zeroBattle = monsterDefeatedSource({aPresent, bExited});
  zeroBattle.battleId = BattleInstanceId{0};
  if (!rejects(zeroBattle)) {
    return false;
  }

  auto zeroCatalog = monsterDefeatedSource({aPresent, bExited});
  zeroCatalog.catalogVersion = 0;
  if (!rejects(zeroCatalog)) {
    return false;
  }

  if (!rejects(monsterDefeatedSource({aPresent}))) {
    return false;
  }

  std::vector<SettlementParticipantSource> tooMany;
  for (std::uint8_t suffix = 1; suffix <= 11; ++suffix) {
    tooMany.push_back(participantSource(
        accountSuffix(suffix), ParticipantExitStatus::TerminalPresent, {}, 0));
  }
  if (!rejects(monsterDefeatedSource(std::move(tooMany)))) {
    return false;
  }

  auto duplicateAccount = monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{1, 1}}, 100),
       participantSource(kAccountA, ParticipantExitStatus::TerminalExited, {},
                         0)});
  if (!rejects(duplicateAccount)) {
    return false;
  }

  auto zeroItemId = monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{0, 1}}, 100),
       bExited});
  if (!rejects(zeroItemId)) {
    return false;
  }

  auto duplicateItemId = monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{1, 1}, {1, 2}}, 300),
       bExited});
  if (!rejects(duplicateItemId)) {
    return false;
  }

  auto zeroQuantity = monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{1, 0}}, 0),
       bExited});
  if (!rejects(zeroQuantity)) {
    return false;
  }

  std::vector<ItemDelta> tooManyItems;
  tooManyItems.reserve(65536);
  for (std::uint64_t itemId = 1; itemId <= 65536; ++itemId) {
    tooManyItems.push_back(ItemDelta{.itemId = itemId, .quantity = 1});
  }
  auto itemCountOverflow = monsterDefeatedSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         std::move(tooManyItems), 0),
       bExited});
  if (!rejects(itemCountOverflow)) {
    return false;
  }

  auto timeoutWithItems = combatTimeoutSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent,
                         {{1, 1}}, 0),
       bExited});
  if (!rejects(timeoutWithItems)) {
    return false;
  }

  auto timeoutWithValue = combatTimeoutSource(
      {participantSource(kAccountA, ParticipantExitStatus::TerminalPresent, {},
                         50),
       bExited});
  if (!rejects(timeoutWithValue)) {
    return false;
  }

  auto invalidOutcomeZero = monsterDefeatedSource({aPresent, bExited});
  invalidOutcomeZero.outcome = static_cast<BattleOutcome>(0);
  if (!rejects(invalidOutcomeZero)) {
    return false;
  }

  auto invalidOutcomeFour = monsterDefeatedSource({aPresent, bExited});
  invalidOutcomeFour.outcome = static_cast<BattleOutcome>(4);
  if (!rejects(invalidOutcomeFour)) {
    return false;
  }

  auto invalidExitStatusZero = monsterDefeatedSource(
      {participantSource(kAccountA, static_cast<ParticipantExitStatus>(0),
                         {{2, 1}}, 300),
       bExited});
  if (!rejects(invalidExitStatusZero)) {
    return false;
  }

  auto invalidExitStatusThree = monsterDefeatedSource(
      {participantSource(kAccountA, static_cast<ParticipantExitStatus>(3),
                         {{2, 1}}, 300),
       bExited});
  return rejects(invalidExitStatusThree);
}

} // namespace

int main() {
  if (!goldenMonsterDefeatedBatch() || !goldenCombatTimeoutBatch() ||
      !goldenCancelledNoHoldingsBatch() ||
      !goldenCancelledPartialHoldingsBatch() ||
      !cancellationRequiresEveryParticipantExited() ||
      !permutationsNormalizeIdentically() ||
      !oneIntentPerSourceAndExitStatusPreserved() ||
      !sameIdentityKeepsIdAndChangesHash() || !invalidSourcesReturnNoBatch()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
