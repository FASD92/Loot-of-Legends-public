#include <lol/battle_continuity/RecoveryStateCodec.hpp>

#include <limits>
#include <utility>

namespace lol::battle_continuity {
namespace {

using ByteView = std::span<const std::uint8_t>;

struct Writer final {
  Bytes bytes;

  void u8(std::uint8_t value) { bytes.push_back(value); }

  void u16(std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  void u32(std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  void u64(std::uint64_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 56U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 48U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 40U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 32U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  void bytesFrom(ByteView value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
};

struct Reader final {
  ByteView bytes;
  std::size_t position{0U};
  std::optional<RecoveryStateCodecError> error;

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes.size() - position;
  }

  [[nodiscard]] bool take(std::size_t count, ByteView &value) noexcept {
    if (count > remaining()) {
      fail(RecoveryStateCodecErrorCode::Truncated, position);
      return false;
    }
    value = bytes.subspan(position, count);
    position += count;
    return true;
  }

  [[nodiscard]] bool u8(std::uint8_t &value) noexcept {
    ByteView view;
    if (!take(1U, view)) {
      return false;
    }
    value = view[0];
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t &value) noexcept {
    ByteView view;
    if (!take(2U, view)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(view[0]) << 8U) |
        static_cast<std::uint32_t>(view[1]));
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t &value) noexcept {
    ByteView view;
    if (!take(4U, view)) {
      return false;
    }
    value = (static_cast<std::uint32_t>(view[0]) << 24U) |
            (static_cast<std::uint32_t>(view[1]) << 16U) |
            (static_cast<std::uint32_t>(view[2]) << 8U) |
            static_cast<std::uint32_t>(view[3]);
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t &value) noexcept {
    ByteView view;
    if (!take(8U, view)) {
      return false;
    }
    value = (static_cast<std::uint64_t>(view[0]) << 56U) |
            (static_cast<std::uint64_t>(view[1]) << 48U) |
            (static_cast<std::uint64_t>(view[2]) << 40U) |
            (static_cast<std::uint64_t>(view[3]) << 32U) |
            (static_cast<std::uint64_t>(view[4]) << 24U) |
            (static_cast<std::uint64_t>(view[5]) << 16U) |
            (static_cast<std::uint64_t>(view[6]) << 8U) |
            static_cast<std::uint64_t>(view[7]);
    return true;
  }

  [[nodiscard]] bool bytesTo(std::size_t count, Bytes &value) noexcept {
    ByteView view;
    if (!take(count, view)) {
      return false;
    }
    value.assign(view.begin(), view.end());
    return true;
  }

  void fail(RecoveryStateCodecErrorCode code, std::size_t at) noexcept {
    if (!error.has_value()) {
      error = RecoveryStateCodecError{.code = code, .offset = at};
    }
  }
};

[[nodiscard]] bool validPhase(RoomRecoveryPhase phase) noexcept {
  switch (phase) {
  case RoomRecoveryPhase::Open:
  case RoomRecoveryPhase::Loading:
  case RoomRecoveryPhase::InProgress:
  case RoomRecoveryPhase::AwaitingSettlementDurability:
    return true;
  }
  return false;
}

[[nodiscard]] std::optional<RecoveryStateCodecErrorCode>
validateRoomState(const RoomRecoveryState &state) noexcept {
  const auto roomId = state.roomId.value();
  if (roomId == 0U || static_cast<std::uint32_t>(roomId >> 32U) == 0U ||
      static_cast<std::uint32_t>(roomId) == 0U) {
    return RecoveryStateCodecErrorCode::InvalidRoomId;
  }
  if (state.capacity < 2U || state.capacity > 10U) {
    return RecoveryStateCodecErrorCode::InvalidCapacity;
  }
  if (state.memberSlots.size() > state.capacity) {
    return RecoveryStateCodecErrorCode::InvalidMemberCount;
  }
  std::uint16_t previousSlot = 0U;
  for (const auto slot : state.memberSlots) {
    if (slot == 0U || slot > state.capacity) {
      return RecoveryStateCodecErrorCode::InvalidMemberSlot;
    }
    if (slot <= previousSlot) {
      return RecoveryStateCodecErrorCode::InvalidMemberOrder;
    }
    previousSlot = slot;
  }
  if (state.memberSlots.empty()) {
    if (state.hostParticipantSlot != 0U) {
      return RecoveryStateCodecErrorCode::InvalidHostParticipantSlot;
    }
  } else if (state.hostParticipantSlot != state.memberSlots.front()) {
    return RecoveryStateCodecErrorCode::InvalidHostParticipantSlot;
  }
  if (!validPhase(state.phase)) {
    return RecoveryStateCodecErrorCode::InvalidPhase;
  }
  if (state.nextBattleOrdinal == 0U) {
    return RecoveryStateCodecErrorCode::InvalidNextBattleOrdinal;
  }
  return std::nullopt;
}

RoomRecoveryStateDecodeResult decodeRoomPayload(ByteView encodedState) {
  RoomRecoveryStateDecodeResult result;
  Reader reader{
      .bytes = encodedState, .position = 0U, .error = std::nullopt};
  std::uint32_t magic = 0U;
  std::uint16_t schema = 0U;
  std::uint64_t roomId = 0U;
  std::uint8_t capacity = 0U;
  std::uint16_t host = 0U;
  std::uint16_t memberCount = 0U;
  std::uint8_t rawPhase = 0U;
  std::uint64_t ordinal = 0U;
  if (!reader.u32(magic) || !reader.u16(schema)) {
    result.error = reader.error;
    return result;
  }
  if (magic != kRoomRecoveryStateMagic) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::InvalidMagic, .offset = 0U};
    return result;
  }
  if (schema != kRoomRecoveryStateSchemaVersion) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::UnsupportedSchemaVersion,
        .offset = 4U};
    return result;
  }
  if (!reader.u64(roomId) || !reader.u8(capacity) || !reader.u16(host) ||
      !reader.u16(memberCount)) {
    result.error = reader.error;
    return result;
  }
  if (memberCount > 10U) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::InvalidMemberCount,
        .offset = reader.position - sizeof(memberCount)};
    return result;
  }
  RoomRecoveryState state{.roomId = shared::RoomId{roomId},
                          .capacity = capacity,
                          .hostParticipantSlot = host,
                          .memberSlots = {},
                          .phase = RoomRecoveryPhase::Open,
                          .nextBattleOrdinal = 0U};
  state.memberSlots.reserve(memberCount);
  for (std::uint16_t index = 0U; index < memberCount; ++index) {
    std::uint16_t slot = 0U;
    if (!reader.u16(slot)) {
      result.error = reader.error;
      return result;
    }
    state.memberSlots.push_back(slot);
  }
  if (!reader.u8(rawPhase) || !reader.u64(ordinal)) {
    result.error = reader.error;
    return result;
  }
  state.phase = static_cast<RoomRecoveryPhase>(rawPhase);
  state.nextBattleOrdinal = ordinal;
  if (reader.position != encodedState.size()) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::TrailingBytes,
        .offset = reader.position};
    return result;
  }
  if (const auto error = validateRoomState(state); error.has_value()) {
    result.error = RecoveryStateCodecError{.code = *error, .offset = 0U};
    return result;
  }
  result.state = std::move(state);
  return result;
}

} // namespace

RoomRecoveryStateEncodeResult
encodeRoomRecoveryState(const RoomRecoveryState &state) {
  RoomRecoveryStateEncodeResult result;
  if (const auto error = validateRoomState(state); error.has_value()) {
    result.error = RecoveryStateCodecError{.code = *error, .offset = 0U};
    return result;
  }
  Writer writer;
  writer.bytes.reserve(32U + state.memberSlots.size() * sizeof(std::uint16_t));
  writer.u32(kRoomRecoveryStateMagic);
  writer.u16(kRoomRecoveryStateSchemaVersion);
  writer.u64(state.roomId.value());
  writer.u8(state.capacity);
  writer.u16(state.hostParticipantSlot);
  writer.u16(static_cast<std::uint16_t>(state.memberSlots.size()));
  for (const auto slot : state.memberSlots) {
    writer.u16(slot);
  }
  writer.u8(static_cast<std::uint8_t>(state.phase));
  writer.u64(state.nextBattleOrdinal);
  result.bytes = std::move(writer.bytes);
  return result;
}

RoomRecoveryStateDecodeResult
decodeRoomRecoveryState(ByteView encodedState) {
  if (encodedState.size() > kMaximumBattleStateBytes) {
    return RoomRecoveryStateDecodeResult{
        .state = std::nullopt,
        .error = RecoveryStateCodecError{
            .code = RecoveryStateCodecErrorCode::StateTooLarge, .offset = 0U}};
  }
  return decodeRoomPayload(encodedState);
}

RecoveryStateEncodeResult encodeRecoveryState(
    const battle::BattleDeterministicState &battleState,
    const RoomRecoveryState &roomState) {
  RecoveryStateEncodeResult result;
  if (battleState.roomId != roomState.roomId) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::RoomIdMismatch, .offset = 0U};
    return result;
  }
  const auto encodedBattle = encodeBattleState(battleState);
  if (!encodedBattle.ok()) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::BattleStateRejected, .offset = 0U};
    return result;
  }
  const auto encodedRoom = encodeRoomRecoveryState(roomState);
  if (!encodedRoom.ok()) {
    result.error = encodedRoom.error;
    return result;
  }
  const auto battleId = battleState.battleId.value();
  if (battleId == std::numeric_limits<std::uint64_t>::max() ||
      roomState.nextBattleOrdinal != battleId + 1U) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::InvalidNextBattleOrdinal,
        .offset = 0U};
    return result;
  }
  Writer writer;
  writer.bytes.reserve(14U + encodedBattle.bytes.size() +
                       encodedRoom.bytes.size());
  writer.u32(kRecoveryStateMagic);
  writer.u16(kRecoveryStateSchemaVersion);
  writer.u32(static_cast<std::uint32_t>(encodedBattle.bytes.size()));
  writer.bytesFrom(encodedBattle.bytes);
  writer.u32(static_cast<std::uint32_t>(encodedRoom.bytes.size()));
  writer.bytesFrom(encodedRoom.bytes);
  if (writer.bytes.size() > kMaximumBattleStateBytes) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::StateTooLarge, .offset = 0U};
    return result;
  }
  result.bytes = std::move(writer.bytes);
  return result;
}

RecoveryStateDecodeResult
decodeRecoveryState(ByteView encodedState) {
  RecoveryStateDecodeResult result;
  if (encodedState.size() > kMaximumBattleStateBytes) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::StateTooLarge, .offset = 0U};
    return result;
  }
  Reader reader{
      .bytes = encodedState, .position = 0U, .error = std::nullopt};
  std::uint32_t magic = 0U;
  std::uint16_t schema = 0U;
  std::uint32_t battleLength = 0U;
  std::uint32_t roomLength = 0U;
  Bytes battleBytes;
  Bytes roomBytes;
  if (!reader.u32(magic) || !reader.u16(schema)) {
    result.error = reader.error;
    return result;
  }
  if (magic != kRecoveryStateMagic) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::InvalidMagic, .offset = 0U};
    return result;
  }
  if (schema != kRecoveryStateSchemaVersion) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::UnsupportedSchemaVersion,
        .offset = 4U};
    return result;
  }
  if (!reader.u32(battleLength) || battleLength > kMaximumBattleStateBytes ||
      !reader.bytesTo(battleLength, battleBytes) ||
      !reader.u32(roomLength) || roomLength > kMaximumBattleStateBytes ||
      !reader.bytesTo(roomLength, roomBytes)) {
    result.error = reader.error;
    if (!result.error.has_value()) {
      result.error = RecoveryStateCodecError{
          .code = RecoveryStateCodecErrorCode::InvalidLength,
          .offset = reader.position};
    }
    return result;
  }
  if (reader.position != encodedState.size()) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::TrailingBytes,
        .offset = reader.position};
    return result;
  }
  const auto decodedBattle = decodeBattleState(battleBytes);
  if (!decodedBattle.ok() || !decodedBattle.state.has_value()) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::BattleStateRejected, .offset = 0U};
    return result;
  }
  const auto decodedRoom = decodeRoomRecoveryState(roomBytes);
  if (!decodedRoom.ok() || !decodedRoom.state.has_value()) {
    result.error = decodedRoom.error;
    return result;
  }
  if (decodedBattle.state->roomId != decodedRoom.state->roomId) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::RoomIdMismatch, .offset = 0U};
    return result;
  }
  const auto battleId = decodedBattle.state->battleId.value();
  if (battleId == std::numeric_limits<std::uint64_t>::max() ||
      decodedRoom.state->nextBattleOrdinal != battleId + 1U) {
    result.error = RecoveryStateCodecError{
        .code = RecoveryStateCodecErrorCode::InvalidNextBattleOrdinal,
        .offset = 0U};
    return result;
  }
  result.state = RecoveryState{.battle = std::move(*decodedBattle.state),
                               .room = std::move(*decodedRoom.state)};
  return result;
}

std::optional<Hash>
recoveryStateHash(ByteView encodedState) noexcept {
  return canonicalStateHash(encodedState);
}

BattleResultEncodeResult encodeCanonicalRecoveryTerminalState(
    ByteView canonicalRecoveryStateBytes,
    const TerminalReceiptPayload &terminalReceipt) {
  BattleResultEncodeResult result;
  const auto decoded = decodeRecoveryState(canonicalRecoveryStateBytes);
  if (!decoded.ok() || !decoded.state.has_value()) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::InvalidIdentity, .offset = 0U};
    return result;
  }
  const auto battleBytes = encodeBattleState(decoded.state->battle);
  if (!battleBytes.ok()) {
    result.error = battleBytes.error;
    return result;
  }
  const auto battleTerminal =
      encodeCanonicalTerminalState(battleBytes.bytes, terminalReceipt);
  if (!battleTerminal.ok() ||
      battleTerminal.bytes.size() < battleBytes.bytes.size()) {
    result.error = battleTerminal.error;
    return result;
  }
  result.bytes.assign(canonicalRecoveryStateBytes.begin(),
                      canonicalRecoveryStateBytes.end());
  result.bytes.insert(result.bytes.end(),
                      battleTerminal.bytes.begin() +
                          static_cast<std::ptrdiff_t>(battleBytes.bytes.size()),
                      battleTerminal.bytes.end());
  if (result.bytes.size() > kMaximumBattleStateBytes) {
    result.bytes.clear();
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::StateTooLarge, .offset = 0U};
  }
  return result;
}

std::optional<Hash> canonicalRecoveryTerminalHash(
    ByteView canonicalRecoveryStateBytes,
    const TerminalReceiptPayload &terminalReceipt) {
  const auto encoded = encodeCanonicalRecoveryTerminalState(
      canonicalRecoveryStateBytes, terminalReceipt);
  if (!encoded.ok()) {
    return std::nullopt;
  }
  return canonicalStateHash(encoded.bytes);
}

} // namespace lol::battle_continuity
