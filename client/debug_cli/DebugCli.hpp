#pragma once

#include "debug_cli/DebugCliCommand.hpp"
#include "debug_cli/DebugClientTransport.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Client {

struct DebugDropState {
    uint32_t dropId{0};
    uint32_t itemId{0};
    uint16_t quantity{0};
};

struct DebugInventoryEntryState {
    uint32_t itemId{0};
    uint16_t quantity{0};
};

struct DebugSettlementInventoryDeltaState {
    uint32_t itemId{0};
    int32_t quantityDelta{0};
    uint32_t sourceDropId{0};
};

struct DebugSettlementState {
    std::string settlementId;
    uint64_t sessionId{0};
    uint64_t accountId{0};
    uint32_t roomId{0};
    uint64_t startedAtUnixMs{0};
    uint64_t finishedAtUnixMs{0};
    int64_t goldDelta{0};
    uint16_t reason{0};
    std::vector<DebugSettlementInventoryDeltaState> inventoryDeltas;
};

struct DebugClientState {
    std::string alias;  // 사용자가 붙인 별칭
    std::string host;   // 접속 대상 서버 host
    uint16_t port{0};   // 접속 대상 port
    bool connected{false};
    std::optional<uint64_t> sessionId;  // 서버에서 받은 세션 ID. 아직 없으면 empty.
    std::optional<uint32_t> roomId;
    uint16_t playerCount{0};
    uint16_t readyPlayerCount{0};
    uint16_t totalPlayerCount{0};
    bool battleStarted{false};
    std::optional<uint32_t> monsterId;
    std::vector<DebugDropState> drops;
    bool hasInventorySnapshot{false};
    uint16_t currentWeight{0};
    uint16_t maxWeight{0};
    std::vector<DebugInventoryEntryState> inventory;
    std::optional<DebugSettlementState> settlement; // 서버에서 마지막으로 받은 settlementResult.
};

struct DebugCliResult {
    bool success{false};
    bool shouldQuit{false};
    std::string output;
};

using DebugClientTransportFactory = std::function<DebugClientTransportPtr()>;

class DebugCli {
public:
    DebugCli();
    explicit DebugCli(DebugClientTransportFactory transportFactory);

    DebugCliResult executeLine(const std::string& line);
    std::vector<DebugClientState> clientStates() const;

private:
    struct ClientSlot {
        DebugClientState state;
        DebugClientTransportPtr transport;
    };

    DebugCliResult handleConnect(const DebugCliCommand& command);
    DebugCliResult handleDisconnect(const DebugCliCommand& command);
    DebugCliResult handleClients() const;
    DebugCliResult handleAliasCommand(const DebugCliCommand& command);
    bool drainAllClients(std::vector<std::string>& outputs, std::string& outError);
    bool drainClient(ClientSlot& slot, std::vector<std::string>& outputs, std::string& outError);

    DebugClientTransportFactory transportFactory_;
    std::map<std::string, ClientSlot> clients_;
};

std::string debugCliHelpText();

} // namespace Client
