#include "debug_cli/DebugCliCommand.hpp"

#include <cctype>
#include <limits>
#include <sstream>
#include <string>

namespace Client {
namespace {

bool hasExtraToken(std::istringstream& stream) {
    std::string extra;
    return static_cast<bool>(stream >> extra);
}
// 문자열을 unsigned 정수로 파싱
bool parseUnsigned(const std::string& text, uint64_t maxValue, uint64_t& outValue) {
    if (text.empty()) {
        return false;
    }

    uint64_t value = 0;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {    // isdigit이 음수를 받는 UB를 방지하기 위해
            return false;
        }

        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (value > (maxValue - digit) / 10) {
            return false;
        }
        value = (value * 10) + digit;
    }

    outValue = value;
    return true;
}

DebugCliCommand invalidCommand(const std::string& message) {
    DebugCliCommand command;
    command.kind = DebugCliCommandKind::kInvalid;
    command.error = message;
    return command;
}

DebugCliCommand parseConnectCommand(std::istringstream& stream) {
    DebugCliCommand command;
    command.kind = DebugCliCommandKind::kConnect;

    std::string portText;
    if (!(stream >> command.alias >> command.host >> portText)) {
        return invalidCommand("usage: connect <alias> <host> <port>");
    }
    if (hasExtraToken(stream)) {
        return invalidCommand("usage: connect <alias> <host> <port>");
    }

    uint64_t parsedPort = 0;
    if (!parseUnsigned(portText, std::numeric_limits<uint16_t>::max(), parsedPort) || parsedPort == 0) {
        return invalidCommand("port must be between 1 and 65535");
    }

    command.port = static_cast<uint16_t>(parsedPort);
    return command;
}

DebugCliCommand parseDisconnectCommand(std::istringstream& stream) {
    DebugCliCommand command;
    command.kind = DebugCliCommandKind::kDisconnect;

    if (!(stream >> command.alias)) {
        return invalidCommand("usage: disconnect <alias>");
    }
    if (hasExtraToken(stream)) {
        return invalidCommand("usage: disconnect <alias>");
    }

    return command;
}

DebugCliAliasCommandKind aliasCommandKindFromToken(const std::string& token) {
    if (token == "create_room") {
        return DebugCliAliasCommandKind::kCreateRoom;
    }
    if (token == "join_room") {
        return DebugCliAliasCommandKind::kJoinRoom;
    }
    if (token == "leave_room") {
        return DebugCliAliasCommandKind::kLeaveRoom;
    }
    if (token == "ready") {
        return DebugCliAliasCommandKind::kReady;
    }
    if (token == "debug_defeat_monster") {
        return DebugCliAliasCommandKind::kDebugDefeatMonster;
    }
    if (token == "click_loot") {
        return DebugCliAliasCommandKind::kClickLoot;
    }
    if (token == "finish_session") {
        return DebugCliAliasCommandKind::kFinishSession;
    }
    if (token == "print_inventory") {
        return DebugCliAliasCommandKind::kPrintInventory;
    }
    if (token == "print_settlement") {
        return DebugCliAliasCommandKind::kPrintSettlement;
    }
    if (token == "print_state") {
        return DebugCliAliasCommandKind::kPrintState;
    }

    return DebugCliAliasCommandKind::kUnknown;
}

bool aliasCommandNeedsRoomId(DebugCliAliasCommandKind kind) {
    return kind == DebugCliAliasCommandKind::kJoinRoom;
}

bool aliasCommandNeedsMonsterId(DebugCliAliasCommandKind kind) {
    return kind == DebugCliAliasCommandKind::kDebugDefeatMonster;
}

bool aliasCommandNeedsDropId(DebugCliAliasCommandKind kind) {
    return kind == DebugCliAliasCommandKind::kClickLoot;
}

DebugCliCommand parseAliasCommand(const std::string& alias, const std::string& commandToken, std::istringstream& stream) {
    DebugCliCommand command;
    command.kind = DebugCliCommandKind::kAliasCommand;
    command.alias = alias;
    command.aliasCommandKind = aliasCommandKindFromToken(commandToken);

    if (command.aliasCommandKind == DebugCliAliasCommandKind::kUnknown) {
        return invalidCommand("unknown alias command: " + commandToken);
    }

    if (aliasCommandNeedsRoomId(command.aliasCommandKind)) {
        std::string roomIdText;
        if (!(stream >> roomIdText)) {
            return invalidCommand("usage: <alias> join_room <roomId>");
        }

        uint64_t roomId = 0;
        if (!parseUnsigned(roomIdText, std::numeric_limits<uint32_t>::max(), roomId)) {
            return invalidCommand("roomId must be a u32");
        }
        command.roomId = static_cast<uint32_t>(roomId);
    } else if (aliasCommandNeedsMonsterId(command.aliasCommandKind)) {
        std::string monsterIdText;
        if (!(stream >> monsterIdText)) {
            return invalidCommand("usage: <alias> debug_defeat_monster <monsterId>");
        }

        uint64_t monsterId = 0;
        if (!parseUnsigned(monsterIdText, std::numeric_limits<uint32_t>::max(), monsterId)) {
            return invalidCommand("monsterId must be a u32");
        }
        command.monsterId = static_cast<uint32_t>(monsterId);
    } else if (aliasCommandNeedsDropId(command.aliasCommandKind)) {
        std::string dropIdText;
        if (!(stream >> dropIdText)) {
            return invalidCommand("usage: <alias> click_loot <dropId>");
        }

        uint64_t dropId = 0;
        if (!parseUnsigned(dropIdText, std::numeric_limits<uint32_t>::max(), dropId)) {
            return invalidCommand("dropId must be a u32");
        }
        command.dropId = static_cast<uint32_t>(dropId);
    }

    if (hasExtraToken(stream)) {
        return invalidCommand("too many arguments for alias command");
    }

    return command;
}

} // namespace

DebugCliCommand parseDebugCliCommand(const std::string& line) {
    std::istringstream stream(line);

    std::string firstToken;
    if (!(stream >> firstToken)) {
        return invalidCommand("empty command");
    }

    if (firstToken == "help") {
        if (hasExtraToken(stream)) {
            return invalidCommand("usage: help");
        }
        DebugCliCommand command;
        command.kind = DebugCliCommandKind::kHelp;
        return command;
    }

    if (firstToken == "quit" || firstToken == "exit") {
        if (hasExtraToken(stream)) {
            return invalidCommand("usage: quit");
        }
        DebugCliCommand command;
        command.kind = DebugCliCommandKind::kQuit;
        return command;
    }

    if (firstToken == "clients") {
        if (hasExtraToken(stream)) {
            return invalidCommand("usage: clients");
        }
        DebugCliCommand command;
        command.kind = DebugCliCommandKind::kClients;
        return command;
    }

    if (firstToken == "connect") {
        return parseConnectCommand(stream);
    }

    if (firstToken == "disconnect") {
        return parseDisconnectCommand(stream);
    }

    std::string aliasCommandToken;
    if (!(stream >> aliasCommandToken)) {
        return invalidCommand("usage: <alias> <command> [args]");
    }

    return parseAliasCommand(firstToken, aliasCommandToken, stream);
}

std::string toString(DebugCliCommandKind kind) {
    switch (kind) {
        case DebugCliCommandKind::kHelp:
            return "help";
        case DebugCliCommandKind::kConnect:
            return "connect";
        case DebugCliCommandKind::kDisconnect:
            return "disconnect";
        case DebugCliCommandKind::kClients:
            return "clients";
        case DebugCliCommandKind::kQuit:
            return "quit";
        case DebugCliCommandKind::kAliasCommand:
            return "alias_command";
        case DebugCliCommandKind::kInvalid:
            return "invalid";
    }

    return "invalid";
}

std::string toString(DebugCliAliasCommandKind kind) {
    switch (kind) {
        case DebugCliAliasCommandKind::kCreateRoom:
            return "create_room";
        case DebugCliAliasCommandKind::kJoinRoom:
            return "join_room";
        case DebugCliAliasCommandKind::kLeaveRoom:
            return "leave_room";
        case DebugCliAliasCommandKind::kReady:
            return "ready";
        case DebugCliAliasCommandKind::kDebugDefeatMonster:
            return "debug_defeat_monster";
        case DebugCliAliasCommandKind::kClickLoot:
            return "click_loot";
        case DebugCliAliasCommandKind::kFinishSession:
            return "finish_session";
        case DebugCliAliasCommandKind::kPrintInventory:
            return "print_inventory";
        case DebugCliAliasCommandKind::kPrintSettlement:
            return "print_settlement";
        case DebugCliAliasCommandKind::kPrintState:
            return "print_state";
        case DebugCliAliasCommandKind::kUnknown:
            return "unknown";
    }

    return "unknown";
}

} // namespace Client
