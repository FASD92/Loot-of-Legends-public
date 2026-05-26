#include "debug_cli/DebugCli.hpp"

#include "Net/TcpPacket.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Client {
namespace {

DebugCliResult successResult(std::string output) {
    DebugCliResult result;
    result.success = true;
    result.output = std::move(output);
    return result;
}

DebugCliResult failureResult(std::string output) {
    DebugCliResult result;
    result.success = false;
    result.output = std::move(output);
    return result;
}

DebugClientTransportPtr makeSocketTransport() {
    return std::make_unique<SocketDebugClientTransport>();
}

template <size_t PacketSize>
std::vector<uint8_t> toVector(const std::array<uint8_t, PacketSize>& packet) {
    return std::vector<uint8_t>(packet.begin(), packet.end());
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            out << "\n";
        }
        out << lines[index];
    }
    return out.str();
}

const char* packetTypeName(Net::TcpPacketType type) {
    switch (type) {
        case Net::TcpPacketType::kWelcome:
            return "Welcome";
        case Net::TcpPacketType::kClientListSnapshot:
            return "ClientListSnapshot";
        case Net::TcpPacketType::kCreateRoomRequest:
            return "CreateRoomRequest";
        case Net::TcpPacketType::kCreateRoomResponse:
            return "CreateRoomResponse";
        case Net::TcpPacketType::kJoinRoomRequest:
            return "JoinRoomRequest";
        case Net::TcpPacketType::kJoinRoomResponse:
            return "JoinRoomResponse";
        case Net::TcpPacketType::kLeaveRoomRequest:
            return "LeaveRoomRequest";
        case Net::TcpPacketType::kLeaveRoomResponse:
            return "LeaveRoomResponse";
        case Net::TcpPacketType::kRoomListSnapshot:
            return "RoomListSnapshot";
        case Net::TcpPacketType::kReadyRoomRequest:
            return "ReadyRoomRequest";
        case Net::TcpPacketType::kReadyRoomResponse:
            return "ReadyRoomResponse";
        case Net::TcpPacketType::kBattleStart:
            return "BattleStart";
        case Net::TcpPacketType::kMonsterSpawn:
            return "MonsterSpawn";
        case Net::TcpPacketType::kMonsterDeathRequest:
            return "MonsterDeathRequest";
        case Net::TcpPacketType::kMonsterDeath:
            return "MonsterDeath";
        case Net::TcpPacketType::kDropListSnapshot:
            return "DropListSnapshot";
        case Net::TcpPacketType::kClickLootRequest:
            return "ClickLootRequest";
        case Net::TcpPacketType::kLootResolved:
            return "LootResolved";
        case Net::TcpPacketType::kLootRejected:
            return "LootRejected";
        case Net::TcpPacketType::kInventorySnapshot:
            return "InventorySnapshot";
        case Net::TcpPacketType::kFinishSessionRequest:
            return "FinishSessionRequest";
        case Net::TcpPacketType::kSettlementResult:
            return "SettlementResult";
        case Net::TcpPacketType::kMetaResponse:
            return "MetaResponse";
        case Net::TcpPacketType::kSmokeCreateCenterDropRequest:
            return "SmokeCreateCenterDropRequest";
        case Net::TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest:
            return "SmokePlacePlayersAroundCenterDropRequest";
        case Net::TcpPacketType::kError:
            return "Error";
    }

    return "Unknown";
}

const char* lootRejectReasonName(Net::TcpLootRejectReason reason) {
    switch (reason) {
        case Net::TcpLootRejectReason::kNone:
            return "None";
        case Net::TcpLootRejectReason::kAlreadyClaimed:
            return "AlreadyClaimed";
        case Net::TcpLootRejectReason::kOverweight:
            return "Overweight";
    }

    return "Unknown";
}

const char* settlementReasonName(uint16_t reason) {
    switch (static_cast<Net::TcpSettlementReason>(reason)) {
        case Net::TcpSettlementReason::kNormal:
            return "Normal";
        case Net::TcpSettlementReason::kDisconnect:
            return "Disconnect";
        case Net::TcpSettlementReason::kServerShutdown:
            return "ServerShutdown";
        case Net::TcpSettlementReason::kForcedClose:
            return "ForcedClose";
    }

    return "Unknown";
}

const char* errorCodeName(Net::TcpErrorCode errorCode) {
    switch (errorCode) {
        case Net::TcpErrorCode::kNone:
            return "None";
        case Net::TcpErrorCode::kFull:
            return "Full";
        case Net::TcpErrorCode::kNotFound:
            return "NotFound";
        case Net::TcpErrorCode::kAlreadyInRoom:
            return "AlreadyInRoom";
        case Net::TcpErrorCode::kNotInRoom:
            return "NotInRoom";
    }

    return "Unknown";
}

void resetRoomRuntimeState(DebugClientState& state) {
    state.roomId.reset();
    state.playerCount = 0;
    state.readyPlayerCount = 0;
    state.totalPlayerCount = 0;
    state.battleStarted = false;
    state.monsterId.reset();
    state.drops.clear();
    state.hasInventorySnapshot = false;
    state.currentWeight = 0;
    state.maxWeight = 0;
    state.inventory.clear();
    state.settlement.reset();   // Room 런타임 상태가 초기화되면 이전 정상 결과도 더 이상 현재 Room 상태와 맞지 않는다.
}

void resetSessionState(DebugClientState& state) {
    state.sessionId.reset();
    resetRoomRuntimeState(state);
}

std::string renderInventory(const DebugClientState& state) {
    std::ostringstream out;
    out << state.alias << " inventory";
    if (!state.hasInventorySnapshot) {
        out << ": <pending>";
        return out.str();
    }

    out << " weight=" << state.currentWeight << "/" << state.maxWeight;
    if (state.inventory.empty()) {
        out << " entries=none";
        return out.str();
    }

    out << " entries:";
    for (const DebugInventoryEntryState& entry : state.inventory) {
        out << "\n  itemId=" << entry.itemId << " quantity=" << entry.quantity;
    }
    return out.str();
}

std::string renderSettlement(const DebugClientState& state) {
    std::ostringstream out;
    out << state.alias << " settlement";
    if (!state.settlement.has_value()) {    // 아직 settlementResult를 받은 적이 없으면 pending 출력.
        out << ": <pending>";
        return out.str();
    }

    const DebugSettlementState& settlement = *state.settlement;
    out << " id=" << settlement.settlementId;
    out << " sessionId=" << settlement.sessionId;
    out << " accountId=" << settlement.accountId;
    out << " roomId=" << settlement.roomId;
    out << " goldDelta=" << settlement.goldDelta;
    out << " reason=" << settlementReasonName(settlement.reason);
    out << " deltas=" << settlement.inventoryDeltas.size();
    for (const DebugSettlementInventoryDeltaState& delta : settlement.inventoryDeltas) {
        out << "\n  itemId=" << delta.itemId
            << " quantityDelta=" << delta.quantityDelta
            << " sourceDropId=" << delta.sourceDropId;
    }
    return out.str();
}

std::string renderClientState(const DebugClientState& state) {
    std::ostringstream out;
    out << state.alias << " ";
    out << (state.connected ? "connected" : "disconnected");
    out << " " << state.host << ":" << state.port;
    if (state.sessionId.has_value()) {
        out << " sessionId=" << *state.sessionId;
    } else {
        out << " sessionId=<pending>";
    }
    if (state.roomId.has_value()) {
        out << " roomId=" << *state.roomId;
        out << " players=" << state.playerCount;
        if (state.totalPlayerCount > 0) {
            out << " ready=" << state.readyPlayerCount << "/" << state.totalPlayerCount;
        }
        out << " battleStarted=" << (state.battleStarted ? "true" : "false");
    } else {
        out << " roomId=<none>";
    }
    if (state.monsterId.has_value()) {
        out << " monsterId=" << *state.monsterId;
    }
    out << " drops=" << state.drops.size();
    if (state.hasInventorySnapshot) {
        out << " inventoryWeight=" << state.currentWeight << "/" << state.maxWeight;
    }
    return out.str();
}

bool buildAliasCommandPacket(
    const DebugCliCommand& command,
    std::vector<uint8_t>& outPacket,
    std::string& outError) {
    switch (command.aliasCommandKind) {
        case DebugCliAliasCommandKind::kCreateRoom: {
            std::array<uint8_t, Net::kTcpHeaderSize> packet{};
            if (!Net::serializeCreateRoomRequestPacket(packet)) {
                outError = "failed to serialize CreateRoomRequest";
                return false;
            }
            outPacket = toVector(packet);
            return true;
        }
        case DebugCliAliasCommandKind::kJoinRoom: {
            std::array<uint8_t, Net::kRoomIdPacketSize> packet{};
            if (!Net::serializeJoinRoomRequestPacket(command.roomId, packet)) {
                outError = "failed to serialize JoinRoomRequest";
                return false;
            }
            outPacket = toVector(packet);
            return true;
        }
        case DebugCliAliasCommandKind::kLeaveRoom: {
            std::array<uint8_t, Net::kTcpHeaderSize> packet{};
            if (!Net::serializeLeaveRoomRequestPacket(packet)) {
                outError = "failed to serialize LeaveRoomRequest";
                return false;
            }
            outPacket = toVector(packet);
            return true;
        }
        case DebugCliAliasCommandKind::kReady: {
            std::array<uint8_t, Net::kTcpHeaderSize> packet{};
            if (!Net::serializeReadyRoomRequestPacket(packet)) {
                outError = "failed to serialize ReadyRoomRequest";
                return false;
            }
            outPacket = toVector(packet);
            return true;
        }
        case DebugCliAliasCommandKind::kDebugDefeatMonster: {
            std::array<uint8_t, Net::kMonsterDeathRequestPacketSize> packet{};
            if (!Net::serializeMonsterDeathRequestPacket(command.monsterId, packet)) {
                outError = "failed to serialize MonsterDeathRequest";
                return false;
            }
            outPacket = toVector(packet);
            return true;
        }
        case DebugCliAliasCommandKind::kClickLoot: {
            std::array<uint8_t, Net::kClickLootRequestPacketSize> packet{};
            if (!Net::serializeClickLootRequestPacket(command.dropId, packet)) {
                outError = "failed to serialize ClickLootRequest";
                return false;
            }
            outPacket = toVector(packet);
            return true;
        }
        case DebugCliAliasCommandKind::kFinishSession: {
            std::array<uint8_t, Net::kFinishSessionRequestPacketSize> packet{};
            if (!Net::serializeFinishSessionRequestPacket(packet)) {
                outError = "failed to serialize FinishSessionRequest";
                return false;
            }
            outPacket = toVector(packet);
            return true;
        }
        case DebugCliAliasCommandKind::kPrintInventory:
        case DebugCliAliasCommandKind::kPrintSettlement:
        case DebugCliAliasCommandKind::kPrintState:
        case DebugCliAliasCommandKind::kUnknown:
            outError = "command does not map to a Phase 1 TCP packet";
            return false;
    }

    outError = "unsupported alias command";
    return false;
}

bool applyPacketToState(
    const std::string& alias,
    DebugClientState& state,
    const std::vector<uint8_t>& packet,
    std::string& outOutput,
    std::string& outError) {
    Net::TcpPacketHeader header;
    if (!Net::peekTcpPacketHeader(packet.data(), packet.size(), header)) {
        outError = alias + " received invalid packet header";
        return false;
    }

    switch (header.type) {
        case Net::TcpPacketType::kWelcome: {
            uint64_t sessionId = 0;
            if (!Net::parseWelcomePacket(packet.data(), packet.size(), header, sessionId)) {
                outError = alias + " failed to parse Welcome";
                return false;
            }
            state.sessionId = sessionId;
            outOutput = alias + " Welcome(sessionId=" + std::to_string(sessionId) + ")";
            return true;
        }
        case Net::TcpPacketType::kClientListSnapshot: {
            std::vector<uint64_t> sessionIds;
            if (!Net::parseClientListSnapshotPacket(packet.data(), packet.size(), header, sessionIds)) {
                outError = alias + " failed to parse ClientListSnapshot";
                return false;
            }
            outOutput = alias + " ClientListSnapshot(count=" + std::to_string(sessionIds.size()) + ")";
            return true;
        }
        case Net::TcpPacketType::kCreateRoomResponse: {
            uint32_t roomId = 0;
            uint16_t playerCount = 0;
            if (!Net::parseCreateRoomResponsePacket(packet.data(), packet.size(), header, roomId, playerCount)) {
                outError = alias + " failed to parse CreateRoomResponse";
                return false;
            }
            resetRoomRuntimeState(state);
            state.roomId = roomId;
            state.playerCount = playerCount;
            outOutput = alias + " CreateRoomResponse(roomId=" + std::to_string(roomId) +
                        ", players=" + std::to_string(playerCount) + ")";
            return true;
        }
        case Net::TcpPacketType::kJoinRoomResponse: {
            uint32_t roomId = 0;
            uint16_t playerCount = 0;
            if (!Net::parseJoinRoomResponsePacket(packet.data(), packet.size(), header, roomId, playerCount)) {
                outError = alias + " failed to parse JoinRoomResponse";
                return false;
            }
            resetRoomRuntimeState(state);
            state.roomId = roomId;
            state.playerCount = playerCount;
            outOutput = alias + " JoinRoomResponse(roomId=" + std::to_string(roomId) +
                        ", players=" + std::to_string(playerCount) + ")";
            return true;
        }
        case Net::TcpPacketType::kLeaveRoomResponse: {
            uint32_t roomId = 0;
            if (!Net::parseLeaveRoomResponsePacket(packet.data(), packet.size(), header, roomId)) {
                outError = alias + " failed to parse LeaveRoomResponse";
                return false;
            }
            resetRoomRuntimeState(state);
            outOutput = alias + " LeaveRoomResponse(roomId=" + std::to_string(roomId) + ")";
            return true;
        }
        case Net::TcpPacketType::kRoomListSnapshot: {
            std::vector<Net::TcpRoomEntry> rooms;
            if (!Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, rooms)) {
                outError = alias + " failed to parse RoomListSnapshot";
                return false;
            }
            outOutput = alias + " RoomListSnapshot(count=" + std::to_string(rooms.size()) + ")";
            return true;
        }
        case Net::TcpPacketType::kReadyRoomResponse: {
            uint32_t roomId = 0;
            uint16_t readyPlayerCount = 0;
            uint16_t totalPlayerCount = 0;
            if (!Net::parseReadyRoomResponsePacket(
                    packet.data(),
                    packet.size(),
                    header,
                    roomId,
                    readyPlayerCount,
                    totalPlayerCount)) {
                outError = alias + " failed to parse ReadyRoomResponse";
                return false;
            }
            state.roomId = roomId;
            state.readyPlayerCount = readyPlayerCount;
            state.totalPlayerCount = totalPlayerCount;
            outOutput = alias + " ReadyRoomResponse(roomId=" + std::to_string(roomId) +
                        ", ready=" + std::to_string(readyPlayerCount) +
                        "/" + std::to_string(totalPlayerCount) + ")";
            return true;
        }
        case Net::TcpPacketType::kBattleStart: {
            uint32_t roomId = 0;
            uint64_t playerA = 0;
            uint64_t playerB = 0;
            if (!Net::parseBattleStartPacket(packet.data(), packet.size(), header, roomId, playerA, playerB)) {
                outError = alias + " failed to parse BattleStart";
                return false;
            }
            state.roomId = roomId;
            state.battleStarted = true;
            outOutput = alias + " BattleStart(roomId=" + std::to_string(roomId) +
                        ", players=" + std::to_string(playerA) +
                        "/" + std::to_string(playerB) + ")";
            return true;
        }
        case Net::TcpPacketType::kMonsterSpawn: {
            uint32_t roomId = 0;
            uint32_t monsterId = 0;
            uint32_t monsterTypeId = 0;
            uint16_t maxHp = 0;
            if (!Net::parseMonsterSpawnPacket(
                    packet.data(),
                    packet.size(),
                    header,
                    roomId,
                    monsterId,
                    monsterTypeId,
                    maxHp)) {
                outError = alias + " failed to parse MonsterSpawn";
                return false;
            }
            state.roomId = roomId;
            state.monsterId = monsterId;
            outOutput = alias + " MonsterSpawn(roomId=" + std::to_string(roomId) +
                        ", monsterId=" + std::to_string(monsterId) +
                        ", typeId=" + std::to_string(monsterTypeId) +
                        ", maxHp=" + std::to_string(maxHp) + ")";
            return true;
        }
        case Net::TcpPacketType::kMonsterDeath: {
            uint32_t roomId = 0;
            uint32_t monsterId = 0;
            if (!Net::parseMonsterDeathPacket(packet.data(), packet.size(), header, roomId, monsterId)) {
                outError = alias + " failed to parse MonsterDeath";
                return false;
            }
            if (state.monsterId == monsterId) {
                state.monsterId.reset();
            }
            outOutput = alias + " MonsterDeath(roomId=" + std::to_string(roomId) +
                        ", monsterId=" + std::to_string(monsterId) + ")";
            return true;
        }
        case Net::TcpPacketType::kDropListSnapshot: {
            uint32_t roomId = 0;
            std::vector<Net::TcpDropEntry> drops;
            if (!Net::parseDropListSnapshotPacket(packet.data(), packet.size(), header, roomId, drops)) {
                outError = alias + " failed to parse DropListSnapshot";
                return false;
            }
            state.roomId = roomId;
            state.drops.clear();
            for (const Net::TcpDropEntry& drop : drops) {
                state.drops.push_back(DebugDropState{drop.dropId, drop.itemId, drop.quantity});
            }
            outOutput = alias + " DropListSnapshot(roomId=" + std::to_string(roomId) +
                        ", drops=" + std::to_string(drops.size()) + ")";
            return true;
        }
        case Net::TcpPacketType::kLootResolved: {
            uint32_t roomId = 0;
            uint32_t dropId = 0;
            uint64_t winnerSessionId = 0;
            uint32_t itemId = 0;
            uint16_t quantity = 0;
            if (!Net::parseLootResolvedPacket(
                    packet.data(),
                    packet.size(),
                    header,
                    roomId,
                    dropId,
                    winnerSessionId,
                    itemId,
                    quantity)) {
                outError = alias + " failed to parse LootResolved";
                return false;
            }
            state.drops.erase(
                std::remove_if(
                    state.drops.begin(),
                    state.drops.end(),
                    [dropId](const DebugDropState& drop) {
                        return drop.dropId == dropId;
                    }),
                state.drops.end());
            outOutput = alias + " LootResolved(roomId=" + std::to_string(roomId) +
                        ", dropId=" + std::to_string(dropId) +
                        ", winnerSessionId=" + std::to_string(winnerSessionId) +
                        ", itemId=" + std::to_string(itemId) +
                        ", quantity=" + std::to_string(quantity) + ")";
            return true;
        }
        case Net::TcpPacketType::kLootRejected: {
            uint32_t roomId = 0;
            uint32_t dropId = 0;
            Net::TcpLootRejectReason reason = Net::TcpLootRejectReason::kNone;
            if (!Net::parseLootRejectedPacket(packet.data(), packet.size(), header, roomId, dropId, reason)) {
                outError = alias + " failed to parse LootRejected";
                return false;
            }
            outOutput = alias + " LootRejected(roomId=" + std::to_string(roomId) +
                        ", dropId=" + std::to_string(dropId) +
                        ", reason=" + lootRejectReasonName(reason) + ")";
            return true;
        }
        case Net::TcpPacketType::kInventorySnapshot: {
            uint64_t sessionId = 0;
            uint16_t currentWeight = 0;
            uint16_t maxWeight = 0;
            std::vector<Net::TcpInventoryEntry> entries;
            if (!Net::parseInventorySnapshotPacket(
                    packet.data(),
                    packet.size(),
                    header,
                    sessionId,
                    currentWeight,
                    maxWeight,
                    entries)) {
                outError = alias + " failed to parse InventorySnapshot";
                return false;
            }
            if (!state.sessionId.has_value() || *state.sessionId == sessionId) {
                state.hasInventorySnapshot = true;
                state.currentWeight = currentWeight;
                state.maxWeight = maxWeight;
                state.inventory.clear();
                for (const Net::TcpInventoryEntry& entry : entries) {
                    state.inventory.push_back(DebugInventoryEntryState{entry.itemId, entry.quantity});
                }
            }
            outOutput = alias + " InventorySnapshot(sessionId=" + std::to_string(sessionId) +
                        ", weight=" + std::to_string(currentWeight) +
                        "/" + std::to_string(maxWeight) +
                        ", entries=" + std::to_string(entries.size()) + ")";
            return true;
        }
        case Net::TcpPacketType::kSettlementResult: {
            Net::TcpSettlementResult settlement;
            if (!Net::parseSettlementResultPacket(packet.data(), packet.size(), header, settlement)) {
                outError = alias + " failed to parse SettlementResult";
                return false;
            }
            DebugSettlementState settlementState;
            settlementState.settlementId = settlement.settlementId;
            settlementState.sessionId = settlement.sessionId;
            settlementState.accountId = settlement.accountId;
            settlementState.roomId = settlement.roomId;
            settlementState.startedAtUnixMs = settlement.startedAtUnixMs;
            settlementState.finishedAtUnixMs = settlement.finishedAtUnixMs;
            settlementState.goldDelta = settlement.goldDelta;
            settlementState.reason = static_cast<uint16_t>(settlement.reason);
            settlementState.inventoryDeltas.reserve(settlement.inventoryDeltas.size());
            for (const Net::TcpSettlementInventoryDelta& delta : settlement.inventoryDeltas) {
                settlementState.inventoryDeltas.push_back(
                    DebugSettlementInventoryDeltaState{
                        delta.itemId,
                        delta.quantityDelta,
                        delta.sourceDropId});
            }
            state.settlement = settlementState;
            outOutput = alias + " SettlementResult(settlementId=" + settlement.settlementId +
                        ", roomId=" + std::to_string(settlement.roomId) +
                        ", deltas=" + std::to_string(settlement.inventoryDeltas.size()) + ")";
            return true;
        }
        case Net::TcpPacketType::kMetaResponse: {
            Net::TcpMetaResponse response;
            if (!Net::parseMetaResponsePacket(packet.data(), packet.size(), header, response)) {
                outError = alias + " failed to parse MetaResponse";
                return false;
            }
            outOutput = alias + " MetaResponse(settlementId=" + response.settlementId +
                        ", retryAfterMs=" + std::to_string(response.retryAfterMs) + ")";
            return true;
        }
        case Net::TcpPacketType::kError: {
            Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
            Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
            if (!Net::parseErrorPacket(packet.data(), packet.size(), header, failedType, errorCode)) {
                outError = alias + " failed to parse Error";
                return false;
            }
            outOutput = alias + " Error(failedType=" + packetTypeName(failedType) +
                        ", code=" + errorCodeName(errorCode) + ")";
            return true;
        }
        case Net::TcpPacketType::kCreateRoomRequest:
        case Net::TcpPacketType::kJoinRoomRequest:
        case Net::TcpPacketType::kLeaveRoomRequest:
        case Net::TcpPacketType::kReadyRoomRequest:
        case Net::TcpPacketType::kMonsterDeathRequest:
        case Net::TcpPacketType::kClickLootRequest:
        case Net::TcpPacketType::kFinishSessionRequest:
        case Net::TcpPacketType::kSmokeCreateCenterDropRequest:
        case Net::TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest:
            outOutput = alias + " ignored outbound packet echo(" + packetTypeName(header.type) + ")";
            return true;
    }

    outOutput = alias + " ignored packet(type=" + std::to_string(static_cast<uint16_t>(header.type)) + ")";
    return true;
}

} // namespace

DebugCli::DebugCli()
    : DebugCli(makeSocketTransport) {
}

DebugCli::DebugCli(DebugClientTransportFactory transportFactory)
    : transportFactory_(std::move(transportFactory)) {
}

DebugCliResult DebugCli::executeLine(const std::string& line) {
    const DebugCliCommand command = parseDebugCliCommand(line);
    if (command.kind == DebugCliCommandKind::kInvalid) {
        return failureResult(command.error);
    }

    switch (command.kind) {
        case DebugCliCommandKind::kHelp:
            return successResult(debugCliHelpText());
        case DebugCliCommandKind::kConnect:
            return handleConnect(command);
        case DebugCliCommandKind::kDisconnect:
            return handleDisconnect(command);
        case DebugCliCommandKind::kClients:
            return handleClients();
        case DebugCliCommandKind::kQuit: {
            DebugCliResult result = successResult("bye");
            result.shouldQuit = true;
            return result;
        }
        case DebugCliCommandKind::kAliasCommand:
            return handleAliasCommand(command);
        case DebugCliCommandKind::kInvalid:
            return failureResult(command.error);
    }

    return failureResult("unsupported command");
}

std::vector<DebugClientState> DebugCli::clientStates() const {
    std::vector<DebugClientState> states;
    states.reserve(clients_.size());
    for (const auto& [alias, slot] : clients_) {
        (void)alias;
        states.push_back(slot.state);
    }
    return states;
}

DebugCliResult DebugCli::handleConnect(const DebugCliCommand& command) {
    const auto existing = clients_.find(command.alias);
    if (existing != clients_.end() && existing->second.state.connected) {
        return failureResult("alias already connected: " + command.alias);
    }

    DebugClientTransportPtr transport = transportFactory_ ? transportFactory_() : nullptr;
    if (!transport) {
        return failureResult("transport factory returned null");
    }

    std::string error;
    if (!transport->connectTo(command.host, command.port, error)) {
        return failureResult("connect failed for " + command.alias + ": " + error);
    }

    ClientSlot slot;
    slot.state.alias = command.alias;
    slot.state.host = command.host;
    slot.state.port = command.port;
    slot.state.connected = true;
    slot.transport = std::move(transport);

    clients_[command.alias] = std::move(slot);

    std::vector<std::string> outputs{
        "connected " + command.alias + " " + command.host + ":" + std::to_string(command.port)};
    if (!drainAllClients(outputs, error)) {
        return failureResult(error);
    }
    return successResult(joinLines(outputs));
}

DebugCliResult DebugCli::handleDisconnect(const DebugCliCommand& command) {
    const auto iter = clients_.find(command.alias);
    if (iter == clients_.end()) {
        return failureResult("unknown alias: " + command.alias);
    }

    if (iter->second.transport) {
        iter->second.transport->disconnect();
    }
    iter->second.state.connected = false;
    resetSessionState(iter->second.state);
    return successResult("disconnected " + command.alias);
}

DebugCliResult DebugCli::handleClients() const {
    if (clients_.empty()) {
        return successResult("clients: none");
    }

    std::ostringstream out;
    out << "clients:";
    for (const auto& [alias, slot] : clients_) {
        (void)alias;
        out << "\n" << renderClientState(slot.state);
    }
    return successResult(out.str());
}

DebugCliResult DebugCli::handleAliasCommand(const DebugCliCommand& command) {
    const auto iter = clients_.find(command.alias);
    if (iter == clients_.end()) {
        return failureResult("unknown alias: " + command.alias);
    }
    if (!iter->second.state.connected) {
        return failureResult("alias is disconnected: " + command.alias);
    }

    if (command.aliasCommandKind == DebugCliAliasCommandKind::kPrintState) {
        return successResult(renderClientState(iter->second.state));
    }
    if (command.aliasCommandKind == DebugCliAliasCommandKind::kPrintInventory) {
        return successResult(renderInventory(iter->second.state));
    }
    if (command.aliasCommandKind == DebugCliAliasCommandKind::kPrintSettlement) {
        return successResult(renderSettlement(iter->second.state));
    }

    std::string error;
    std::vector<uint8_t> packet;
    if (!buildAliasCommandPacket(command, packet, error)) {
        return failureResult(error);
    }
    if (!iter->second.transport->sendPacket(packet, error)) {
        return failureResult(command.alias + " send failed: " + error);
    }

    std::vector<std::string> outputs{command.alias + " sent " + toString(command.aliasCommandKind)};
    if (!drainAllClients(outputs, error)) {
        return failureResult(error);
    }
    return successResult(joinLines(outputs));
}

bool DebugCli::drainAllClients(std::vector<std::string>& outputs, std::string& outError) {
    for (auto& [alias, slot] : clients_) {
        (void)alias;
        if (!drainClient(slot, outputs, outError)) {
            return false;
        }
    }
    return true;
}

bool DebugCli::drainClient(ClientSlot& slot, std::vector<std::string>& outputs, std::string& outError) {
    if (!slot.transport || !slot.transport->isConnected()) {
        return true;
    }

    std::vector<std::vector<uint8_t>> packets;
    if (!slot.transport->pollPackets(packets, outError)) {
        slot.state.connected = false;
        return false;
    }

    for (const std::vector<uint8_t>& packet : packets) {
        std::string output;
        if (!applyPacketToState(slot.state.alias, slot.state, packet, output, outError)) {
            return false;
        }
        outputs.push_back(output);
    }
    return true;
}

std::string debugCliHelpText() {
    return
        "global commands:\n"
        "  help\n"
        "  connect <alias> <host> <port>\n"
        "  disconnect <alias>\n"
        "  clients\n"
        "  quit\n"
        "alias commands:\n"
        "  <alias> create_room\n"
        "  <alias> join_room <roomId>\n"
        "  <alias> leave_room\n"
        "  <alias> ready\n"
        "  <alias> debug_defeat_monster <monsterId>\n"
        "  <alias> click_loot <dropId>\n"
        "  <alias> finish_session\n"
        "  <alias> print_inventory\n"
        "  <alias> print_settlement\n"
        "  <alias> print_state";
}

} // namespace Client
