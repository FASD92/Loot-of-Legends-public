if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(lol_server LANGUAGES CXX)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

add_library(
  lol_server_core
  src/Core/ClientConnection.cpp
  src/Core/ClientConnection.hpp
  src/Core/RudpInputCommandRoomEventTranslator.cpp
  src/Core/RudpInputCommandRoomEventTranslator.hpp
  src/Core/Server.cpp
  src/Core/Server.hpp
  src/Core/Session.cpp
  src/Core/Session.hpp
  src/Core/SessionManager.cpp
  src/Core/SessionManager.hpp
  src/Game/Room.cpp
  src/Game/Room.hpp
  src/Game/RoomActor.cpp
  src/Game/RoomActor.hpp
  src/Game/RoomEventDispatcher.cpp
  src/Game/RoomEventDispatcher.hpp
  src/Game/RoomEventMetrics.cpp
  src/Game/RoomEventMetrics.hpp
  src/Game/RoomEventQueue.cpp
  src/Game/RoomEventQueue.hpp
  src/Game/RoomManager.cpp
  src/Game/RoomManager.hpp
  src/Game/OutboundSendQueue.cpp
  src/Game/OutboundSendQueue.hpp
  src/Game/WorkerPool.cpp
  src/Game/WorkerPool.hpp
  src/Net/RudpAckWindow.cpp
  src/Net/RudpAckWindow.hpp
  src/Net/RudpBattleStartPayload.cpp
  src/Net/RudpBattleStartPayload.hpp
  src/Net/RudpDatagramReceiver.cpp
  src/Net/RudpDatagramReceiver.hpp
  src/Net/RudpGameEventPayload.cpp
  src/Net/RudpGameEventPayload.hpp
  src/Net/RudpGameplayEventIdempotencyTracker.cpp
  src/Net/RudpGameplayEventIdempotencyTracker.hpp
  src/Net/RudpHelloPayload.cpp
  src/Net/RudpHelloPayload.hpp
  src/Net/RudpInputCommandPayload.cpp
  src/Net/RudpInputCommandPayload.hpp
  src/Net/RudpInputCommandSequenceTracker.cpp
  src/Net/RudpInputCommandSequenceTracker.hpp
  src/Net/RudpMetaResponseIdempotencyTracker.cpp
  src/Net/RudpMetaResponseIdempotencyTracker.hpp
  src/Net/RudpMetaResponsePayload.cpp
  src/Net/RudpMetaResponsePayload.hpp
  src/Net/RudpMoveInputGuard.cpp
  src/Net/RudpMoveInputGuard.hpp
  src/Net/RudpPacket.cpp
  src/Net/RudpPacket.hpp
  src/Net/RudpPeer.cpp
  src/Net/RudpPeer.hpp
  src/Net/RudpPeerRegistry.cpp
  src/Net/RudpPeerRegistry.hpp
  src/Net/RudpReceivePipeline.cpp
  src/Net/RudpReceivePipeline.hpp
  src/Net/RudpReliableEventSendQueue.cpp
  src/Net/RudpReliableEventSendQueue.hpp
  src/Net/RudpReliableSendQueue.cpp
  src/Net/RudpReliableSendQueue.hpp
  src/Net/RudpRetransmissionFlush.cpp
  src/Net/RudpRetransmissionFlush.hpp
  src/Net/RudpRetransmissionScan.cpp
  src/Net/RudpRetransmissionScan.hpp
  src/Net/RudpSessionBinder.cpp
  src/Net/RudpSessionBinder.hpp
  src/Net/RudpSocketDrain.cpp
  src/Net/RudpSocketDrain.hpp
  src/Net/RudpStateSnapshotPayload.cpp
  src/Net/RudpStateSnapshotPayload.hpp
  src/Net/NetworkEventLoop.hpp
  src/Net/TcpPacket.cpp
  src/Net/TcpPacket.hpp
  src/Net/TcpPacketReader.cpp
  src/Net/TcpPacketReader.hpp
  src/Net/TcpListener.cpp
  src/Net/TcpListener.hpp
  src/Net/UdpSocket.cpp
  src/Net/UdpSocket.hpp
  src/Util/Time.cpp
  src/Util/Time.hpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(
    lol_server_core
    PRIVATE
      src/Platform/Linux/EpollEventLoop.cpp
      src/Platform/Linux/EpollEventLoop.hpp
  )
endif()

target_include_directories(lol_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(lol_server_core PUBLIC cxx_std_17)

add_executable(lol_server src/main.cpp)
target_link_libraries(lol_server PRIVATE lol_server_core)
