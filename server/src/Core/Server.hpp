#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Core/ClientConnection.hpp"
#include "Core/SessionManager.hpp"
#include "Game/RoomManager.hpp"
#include "Net/TcpPacket.hpp"
#include "Net/TcpListener.hpp"
#include "Util/Time.hpp"

namespace Core {
class Server {
public:
    explicit Server(uint16_t port);

    bool start();
    void run();
    void requestStop();
    uint16_t boundPort() const;
    size_t activeConnectionCount() const;
    size_t sessionCount() const;

private:
    void tickOnce();
    void acceptNewClients(Util::TimePoint now);
    void processActiveConnections(Util::TimePoint now);
    std::vector<uint64_t> collectActiveSessionIds() const;
    std::vector<Net::TcpRoomEntry> collectRoomEntries() const;
    bool handleRoomPacket(
        ClientConnection& connection,
        const std::vector<uint8_t>& packet,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    bool sendPacketToClient(
        int clientFd,
        const uint8_t* data,
        size_t size,
        std::vector<int>& disconnectedClients);
    bool broadcastBattleStart(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastMonsterSpawn(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastMonsterDeath(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastDropListSnapshot(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastLootResolved(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool sendLootRejected(
        int clientFd,
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool sendInventorySnapshot(
        int clientFd,
        const Game::InventorySnapshot& inventory,
        std::vector<int>& disconnectedClients);
    void broadcastStateSnapshots(bool clientListChanged, bool roomListChanged);
    bool disconnectClient(int clientFd);
    void closeAllConnections();

    Net::TcpListener listener_;
    SessionManager sessionManager_;
    Game::RoomManager roomManager_;
    std::unordered_map<int, std::unique_ptr<ClientConnection>> connections_;
    std::atomic<size_t> activeConnectionCount_;
    std::atomic<size_t> sessionCountSnapshot_;
    std::atomic<bool> running_;
    uint16_t port_;
};
}  // namespace Core
