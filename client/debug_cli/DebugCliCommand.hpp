#pragma once

#include <cstdint>
#include <string>

namespace Client {

enum class DebugCliCommandKind {
    kHelp,
    kConnect,
    kDisconnect,
    kClients,
    kQuit,
    kAliasCommand,
    kInvalid,
};

enum class DebugCliAliasCommandKind {
    kCreateRoom,
    kJoinRoom,
    kLeaveRoom,
    kReady,
    kDebugDefeatMonster,
    kClickLoot,
    kFinishSession,
    kPrintInventory,
    kPrintSettlement,
    kPrintState,
    kUnknown,
};

struct DebugCliCommand {
    DebugCliCommandKind kind{DebugCliCommandKind::kInvalid};
    DebugCliAliasCommandKind aliasCommandKind{DebugCliAliasCommandKind::kUnknown};
    std::string alias;
    std::string host;
    uint16_t port{0};
    uint32_t roomId{0};
    uint32_t monsterId{0};
    uint32_t dropId{0};
    std::string error;
};

DebugCliCommand parseDebugCliCommand(const std::string& line);
std::string toString(DebugCliCommandKind kind);
std::string toString(DebugCliAliasCommandKind kind);

} // namespace Client
