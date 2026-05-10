#include "debug_cli/DebugCli.hpp"
#include "debug_cli/DebugCliCommand.hpp"

#include "Net/TcpPacket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct FakeTransportState {
    bool shouldConnect{true};
    bool connected{false};
    std::string host;
    uint16_t port{0};
    std::vector<std::vector<uint8_t>> incomingPackets;
    std::vector<std::vector<uint8_t>> sentPackets;
};

class FakeDebugClientTransport final : public Client::IDebugClientTransport {
public:
    explicit FakeDebugClientTransport(std::shared_ptr<FakeTransportState> state)
        : state_(std::move(state)) {
    }

    bool connectTo(const std::string& host, uint16_t port, std::string& outError) override {
        state_->host = host;
        state_->port = port;
        if (!state_->shouldConnect) {
            outError = "fake connect failure";
            return false;
        }

        state_->connected = true;
        return true;
    }

    void disconnect() override {
        state_->connected = false;
    }

    bool isConnected() const override {
        return state_->connected;
    }

    bool sendPacket(const std::vector<uint8_t>& packet, std::string& outError) override {
        if (!state_->connected) {
            outError = "not connected";
            return false;
        }

        state_->sentPackets.push_back(packet);
        return true;
    }

    bool pollPackets(std::vector<std::vector<uint8_t>>& outPackets, std::string& outError) override {
        (void)outError;
        outPackets = std::move(state_->incomingPackets);
        state_->incomingPackets.clear();
        return true;
    }

private:
    std::shared_ptr<FakeTransportState> state_;
};

template <size_t PacketSize>
std::vector<uint8_t> toVector(const std::array<uint8_t, PacketSize>& packet) {
    return std::vector<uint8_t>(packet.begin(), packet.end());
}

std::unique_ptr<Client::IDebugClientTransport> makeSuccessfulFakeTransport() {
    return std::make_unique<FakeDebugClientTransport>(std::make_shared<FakeTransportState>());
}

Client::DebugCli makeCliWithTransport(const std::shared_ptr<FakeTransportState>& state) {
    return Client::DebugCli(
        [state]() {
            return std::make_unique<FakeDebugClientTransport>(state);
        });
}

std::vector<uint8_t> welcomePacket(uint64_t sessionId) {
    std::array<uint8_t, Net::kWelcomePacketSize> packet{};
    EXPECT_TRUE(Net::serializeWelcomePacket(sessionId, packet));
    return toVector(packet);
}

std::vector<uint8_t> createRoomResponsePacket(uint32_t roomId, uint16_t playerCount) {
    std::array<uint8_t, Net::kRoomStatusPacketSize> packet{};
    EXPECT_TRUE(Net::serializeCreateRoomResponsePacket(roomId, playerCount, packet));
    return toVector(packet);
}

std::vector<uint8_t> readyRoomResponsePacket(
    uint32_t roomId,
    uint16_t readyPlayerCount,
    uint16_t totalPlayerCount) {
    std::array<uint8_t, Net::kReadyRoomStatusPacketSize> packet{};
    EXPECT_TRUE(Net::serializeReadyRoomResponsePacket(roomId, readyPlayerCount, totalPlayerCount, packet));
    return toVector(packet);
}

std::vector<uint8_t> battleStartPacket(
    uint32_t roomId,
    uint64_t playerASessionId,
    uint64_t playerBSessionId) {
    std::array<uint8_t, Net::kBattleStartPacketSize> packet{};
    EXPECT_TRUE(Net::serializeBattleStartPacket(roomId, playerASessionId, playerBSessionId, packet));
    return toVector(packet);
}

std::vector<uint8_t> monsterSpawnPacket(
    uint32_t roomId,
    uint32_t monsterId,
    uint32_t monsterTypeId,
    uint16_t maxHp) {
    std::array<uint8_t, Net::kMonsterSpawnPacketSize> packet{};
    EXPECT_TRUE(Net::serializeMonsterSpawnPacket(roomId, monsterId, monsterTypeId, maxHp, packet));
    return toVector(packet);
}

std::vector<uint8_t> monsterDeathPacket(uint32_t roomId, uint32_t monsterId) {
    std::array<uint8_t, Net::kMonsterDeathPacketSize> packet{};
    EXPECT_TRUE(Net::serializeMonsterDeathPacket(roomId, monsterId, packet));
    return toVector(packet);
}

std::vector<uint8_t> dropListSnapshotPacket(uint32_t roomId, const std::vector<Net::TcpDropEntry>& drops) {
    std::vector<uint8_t> packet;
    EXPECT_TRUE(Net::serializeDropListSnapshotPacket(roomId, drops, packet));
    return packet;
}

std::vector<uint8_t> lootResolvedPacket(
    uint32_t roomId,
    uint32_t dropId,
    uint64_t winnerSessionId,
    uint32_t itemId,
    uint16_t quantity) {
    std::array<uint8_t, Net::kLootResolvedPacketSize> packet{};
    EXPECT_TRUE(Net::serializeLootResolvedPacket(roomId, dropId, winnerSessionId, itemId, quantity, packet));
    return toVector(packet);
}

std::vector<uint8_t> inventorySnapshotPacket(
    uint64_t sessionId,
    uint16_t currentWeight,
    uint16_t maxWeight,
    const std::vector<Net::TcpInventoryEntry>& entries) {
    std::vector<uint8_t> packet;
    EXPECT_TRUE(Net::serializeInventorySnapshotPacket(sessionId, currentWeight, maxWeight, entries, packet));
    return packet;
}

std::vector<uint8_t> settlementResultPacket(const Net::TcpSettlementResult& settlement) {
    std::vector<uint8_t> packet;
    EXPECT_TRUE(Net::serializeSettlementResultPacket(settlement, packet));
    return packet;
}

Net::TcpPacketType packetTypeOf(const std::vector<uint8_t>& packet) {
    Net::TcpPacketHeader header;
    EXPECT_TRUE(Net::peekTcpPacketHeader(packet.data(), packet.size(), header));
    return header.type;
}

} // namespace

TEST(DebugCliCommandTests, ParsesGlobalCommands) {
    EXPECT_EQ(Client::parseDebugCliCommand("help").kind, Client::DebugCliCommandKind::kHelp);
    EXPECT_EQ(Client::parseDebugCliCommand("clients").kind, Client::DebugCliCommandKind::kClients);
    EXPECT_EQ(Client::parseDebugCliCommand("quit").kind, Client::DebugCliCommandKind::kQuit);
}

TEST(DebugCliCommandTests, ParsesConnectCommand) {
    const Client::DebugCliCommand command = Client::parseDebugCliCommand("connect A 127.0.0.1 4000");

    EXPECT_EQ(command.kind, Client::DebugCliCommandKind::kConnect);
    EXPECT_EQ(command.alias, "A");
    EXPECT_EQ(command.host, "127.0.0.1");
    EXPECT_EQ(command.port, 4000);
}

TEST(DebugCliCommandTests, RejectsInvalidConnectPort) {
    const Client::DebugCliCommand command = Client::parseDebugCliCommand("connect A 127.0.0.1 70000");

    EXPECT_EQ(command.kind, Client::DebugCliCommandKind::kInvalid);
    EXPECT_FALSE(command.error.empty());
}

TEST(DebugCliCommandTests, ParsesAliasCommandsWithArguments) {
    const Client::DebugCliCommand join = Client::parseDebugCliCommand("A join_room 42");
    EXPECT_EQ(join.kind, Client::DebugCliCommandKind::kAliasCommand);
    EXPECT_EQ(join.alias, "A");
    EXPECT_EQ(join.aliasCommandKind, Client::DebugCliAliasCommandKind::kJoinRoom);
    EXPECT_EQ(join.roomId, 42U);

    const Client::DebugCliCommand monster = Client::parseDebugCliCommand("A debug_defeat_monster 7");
    EXPECT_EQ(monster.aliasCommandKind, Client::DebugCliAliasCommandKind::kDebugDefeatMonster);
    EXPECT_EQ(monster.monsterId, 7U);

    const Client::DebugCliCommand loot = Client::parseDebugCliCommand("A click_loot 9");
    EXPECT_EQ(loot.aliasCommandKind, Client::DebugCliAliasCommandKind::kClickLoot);
    EXPECT_EQ(loot.dropId, 9U);
}

TEST(DebugCliCommandTests, RejectsExtraAliasArguments) {
    const Client::DebugCliCommand command = Client::parseDebugCliCommand("A ready extra");

    EXPECT_EQ(command.kind, Client::DebugCliCommandKind::kInvalid);
    EXPECT_FALSE(command.error.empty());
}

TEST(DebugCliTests, ConnectStoresAliasAndListsClient) {
    Client::DebugCli cli(makeSuccessfulFakeTransport);

    const Client::DebugCliResult connect = cli.executeLine("connect A 127.0.0.1 4000");
    EXPECT_TRUE(connect.success);

    const std::vector<Client::DebugClientState> states = cli.clientStates();
    ASSERT_EQ(states.size(), 1U);
    EXPECT_EQ(states[0].alias, "A");
    EXPECT_EQ(states[0].host, "127.0.0.1");
    EXPECT_EQ(states[0].port, 4000);
    EXPECT_TRUE(states[0].connected);

    const Client::DebugCliResult clients = cli.executeLine("clients");
    EXPECT_TRUE(clients.success);
    EXPECT_NE(clients.output.find("A connected 127.0.0.1:4000"), std::string::npos);
}

TEST(DebugCliTests, ConnectAppliesWelcomePacket) {
    const auto transportState = std::make_shared<FakeTransportState>();
    transportState->incomingPackets.push_back(welcomePacket(1001));
    Client::DebugCli cli = makeCliWithTransport(transportState);

    const Client::DebugCliResult connect = cli.executeLine("connect A 127.0.0.1 4000");
    EXPECT_TRUE(connect.success);
    EXPECT_NE(connect.output.find("Welcome(sessionId=1001)"), std::string::npos);

    const std::vector<Client::DebugClientState> states = cli.clientStates();
    ASSERT_EQ(states.size(), 1U);
    ASSERT_TRUE(states[0].sessionId.has_value());
    EXPECT_EQ(*states[0].sessionId, 1001U);
}

TEST(DebugCliTests, RejectsDuplicateConnectedAlias) {
    Client::DebugCli cli(makeSuccessfulFakeTransport);

    EXPECT_TRUE(cli.executeLine("connect A 127.0.0.1 4000").success);

    const Client::DebugCliResult duplicate = cli.executeLine("connect A 127.0.0.1 4001");
    EXPECT_FALSE(duplicate.success);
    EXPECT_NE(duplicate.output.find("already connected"), std::string::npos);
}

TEST(DebugCliTests, DisconnectMarksClientDisconnected) {
    Client::DebugCli cli(makeSuccessfulFakeTransport);

    EXPECT_TRUE(cli.executeLine("connect A 127.0.0.1 4000").success);
    EXPECT_TRUE(cli.executeLine("disconnect A").success);

    const std::vector<Client::DebugClientState> states = cli.clientStates();
    ASSERT_EQ(states.size(), 1U);
    EXPECT_FALSE(states[0].connected);
}

TEST(DebugCliTests, AliasCommandRequiresKnownConnectedAlias) {
    Client::DebugCli cli(makeSuccessfulFakeTransport);

    const Client::DebugCliResult missing = cli.executeLine("A ready");
    EXPECT_FALSE(missing.success);
    EXPECT_NE(missing.output.find("unknown alias"), std::string::npos);

    EXPECT_TRUE(cli.executeLine("connect A 127.0.0.1 4000").success);
    const Client::DebugCliResult ready = cli.executeLine("A ready");
    EXPECT_TRUE(ready.success);
    EXPECT_NE(ready.output.find("sent ready"), std::string::npos);
}

TEST(DebugCliTests, CreateRoomSendsRequestAndAppliesResponse) {
    const auto transportState = std::make_shared<FakeTransportState>();
    transportState->incomingPackets.push_back(welcomePacket(1001));
    Client::DebugCli cli = makeCliWithTransport(transportState);
    ASSERT_TRUE(cli.executeLine("connect A 127.0.0.1 4000").success);

    transportState->incomingPackets.push_back(createRoomResponsePacket(7, 1));
    const Client::DebugCliResult create = cli.executeLine("A create_room");

    EXPECT_TRUE(create.success);
    ASSERT_EQ(transportState->sentPackets.size(), 1U);
    EXPECT_EQ(packetTypeOf(transportState->sentPackets[0]), Net::TcpPacketType::kCreateRoomRequest);

    const std::vector<Client::DebugClientState> states = cli.clientStates();
    ASSERT_EQ(states.size(), 1U);
    ASSERT_TRUE(states[0].roomId.has_value());
    EXPECT_EQ(*states[0].roomId, 7U);
    EXPECT_EQ(states[0].playerCount, 1U);
}

TEST(DebugCliTests, PhaseOneFlowUpdatesBattleLootAndInventoryState) {
    const auto transportState = std::make_shared<FakeTransportState>();
    transportState->incomingPackets.push_back(welcomePacket(1001));
    Client::DebugCli cli = makeCliWithTransport(transportState);
    ASSERT_TRUE(cli.executeLine("connect A 127.0.0.1 4000").success);

    transportState->incomingPackets.push_back(readyRoomResponsePacket(7, 2, 2));
    transportState->incomingPackets.push_back(battleStartPacket(7, 1001, 1002));
    transportState->incomingPackets.push_back(monsterSpawnPacket(7, 501, 9001, 30));
    EXPECT_TRUE(cli.executeLine("A ready").success);

    std::vector<Client::DebugClientState> states = cli.clientStates();
    ASSERT_EQ(states.size(), 1U);
    EXPECT_TRUE(states[0].battleStarted);
    ASSERT_TRUE(states[0].monsterId.has_value());
    EXPECT_EQ(*states[0].monsterId, 501U);

    transportState->incomingPackets.push_back(monsterDeathPacket(7, 501));
    transportState->incomingPackets.push_back(dropListSnapshotPacket(7, {Net::TcpDropEntry{77, 3001, 2}}));
    EXPECT_TRUE(cli.executeLine("A debug_defeat_monster 501").success);

    states = cli.clientStates();
    ASSERT_EQ(states.size(), 1U);
    ASSERT_EQ(states[0].drops.size(), 1U);
    EXPECT_EQ(states[0].drops[0].dropId, 77U);

    transportState->incomingPackets.push_back(lootResolvedPacket(7, 77, 1001, 3001, 2));
    transportState->incomingPackets.push_back(
        inventorySnapshotPacket(1001, 2, 10, {Net::TcpInventoryEntry{3001, 2}}));
    EXPECT_TRUE(cli.executeLine("A click_loot 77").success);

    states = cli.clientStates();
    ASSERT_EQ(states.size(), 1U);
    EXPECT_TRUE(states[0].drops.empty());
    EXPECT_TRUE(states[0].hasInventorySnapshot);
    EXPECT_EQ(states[0].currentWeight, 2U);
    EXPECT_EQ(states[0].maxWeight, 10U);
    ASSERT_EQ(states[0].inventory.size(), 1U);
    EXPECT_EQ(states[0].inventory[0].itemId, 3001U);
    EXPECT_EQ(states[0].inventory[0].quantity, 2U);

    ASSERT_EQ(transportState->sentPackets.size(), 3U);
    EXPECT_EQ(packetTypeOf(transportState->sentPackets[0]), Net::TcpPacketType::kReadyRoomRequest);
    EXPECT_EQ(packetTypeOf(transportState->sentPackets[1]), Net::TcpPacketType::kMonsterDeathRequest);
    EXPECT_EQ(packetTypeOf(transportState->sentPackets[2]), Net::TcpPacketType::kClickLootRequest);
}

TEST(DebugCliTests, PrintInventoryUsesLatestInventorySnapshot) {
    const auto transportState = std::make_shared<FakeTransportState>();
    transportState->incomingPackets.push_back(welcomePacket(1001));
    Client::DebugCli cli = makeCliWithTransport(transportState);
    ASSERT_TRUE(cli.executeLine("connect A 127.0.0.1 4000").success);

    transportState->incomingPackets.push_back(
        inventorySnapshotPacket(1001, 3, 10, {Net::TcpInventoryEntry{3001, 1}}));
    ASSERT_TRUE(cli.executeLine("A ready").success);

    const Client::DebugCliResult print = cli.executeLine("A print_inventory");
    EXPECT_TRUE(print.success);
    EXPECT_NE(print.output.find("weight=3/10"), std::string::npos);
    EXPECT_NE(print.output.find("itemId=3001"), std::string::npos);
}

TEST(DebugCliTests, FinishSessionSendsRequestAndStoresSettlement) {
    const auto transportState = std::make_shared<FakeTransportState>();
    transportState->incomingPackets.push_back(welcomePacket(1001));
    Client::DebugCli cli = makeCliWithTransport(transportState);
    ASSERT_TRUE(cli.executeLine("connect A 127.0.0.1 4000").success);

    Net::TcpSettlementResult settlement;
    settlement.settlementId = "room-7-session-1001-finish-1";
    settlement.sessionId = 1001;
    settlement.accountId = 1001;
    settlement.roomId = 7;
    settlement.startedAtUnixMs = 100;
    settlement.finishedAtUnixMs = 200;
    settlement.goldDelta = 0;
    settlement.reason = Net::TcpSettlementReason::kNormal;
    settlement.inventoryDeltas.push_back(Net::TcpSettlementInventoryDelta{3001, 2, 77});
    transportState->incomingPackets.push_back(settlementResultPacket(settlement));

    const Client::DebugCliResult finish = cli.executeLine("A finish_session");
    EXPECT_TRUE(finish.success);
    EXPECT_NE(finish.output.find("SettlementResult"), std::string::npos);
    ASSERT_EQ(transportState->sentPackets.size(), 1U);
    EXPECT_EQ(packetTypeOf(transportState->sentPackets[0]), Net::TcpPacketType::kFinishSessionRequest);

    const std::vector<Client::DebugClientState> states = cli.clientStates();
    ASSERT_EQ(states.size(), 1U);
    ASSERT_TRUE(states[0].settlement.has_value());
    EXPECT_EQ(states[0].settlement->settlementId, settlement.settlementId);
    EXPECT_EQ(states[0].settlement->sessionId, settlement.sessionId);
    EXPECT_EQ(states[0].settlement->accountId, settlement.accountId);
    EXPECT_EQ(states[0].settlement->roomId, settlement.roomId);
    ASSERT_EQ(states[0].settlement->inventoryDeltas.size(), 1U);
    EXPECT_EQ(states[0].settlement->inventoryDeltas[0].itemId, 3001U);
    EXPECT_EQ(states[0].settlement->inventoryDeltas[0].quantityDelta, 2);
    EXPECT_EQ(states[0].settlement->inventoryDeltas[0].sourceDropId, 77U);
}

TEST(DebugCliTests, PrintSettlementUsesLatestSettlementResult) {
    const auto transportState = std::make_shared<FakeTransportState>();
    transportState->incomingPackets.push_back(welcomePacket(1001));
    Client::DebugCli cli = makeCliWithTransport(transportState);
    ASSERT_TRUE(cli.executeLine("connect A 127.0.0.1 4000").success);

    Net::TcpSettlementResult settlement;
    settlement.settlementId = "room-7-session-1001-finish-1";
    settlement.sessionId = 1001;
    settlement.accountId = 1001;
    settlement.roomId = 7;
    settlement.startedAtUnixMs = 100;
    settlement.finishedAtUnixMs = 200;
    settlement.reason = Net::TcpSettlementReason::kNormal;
    settlement.inventoryDeltas.push_back(Net::TcpSettlementInventoryDelta{3001, 2, 77});
    transportState->incomingPackets.push_back(settlementResultPacket(settlement));
    ASSERT_TRUE(cli.executeLine("A finish_session").success);

    const Client::DebugCliResult print = cli.executeLine("A print_settlement");
    EXPECT_TRUE(print.success);
    EXPECT_NE(print.output.find("room-7-session-1001-finish-1"), std::string::npos);
    EXPECT_NE(print.output.find("reason=Normal"), std::string::npos);
    EXPECT_NE(print.output.find("quantityDelta=2"), std::string::npos);
    EXPECT_NE(print.output.find("sourceDropId=77"), std::string::npos);
}

TEST(DebugCliTests, QuitSignalsShutdown) {
    Client::DebugCli cli(makeSuccessfulFakeTransport);

    const Client::DebugCliResult quit = cli.executeLine("quit");
    EXPECT_TRUE(quit.success);
    EXPECT_TRUE(quit.shouldQuit);
}
