option(LOL_USE_SYSTEM_GTEST "Use system-installed GoogleTest" OFF)

if(LOL_USE_SYSTEM_GTEST)
  find_package(GTest REQUIRED)
else()
  include(FetchContent)
  set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

add_executable(
  lol_tests
  core/OutboundSendQueueTests.cpp
  core/RoomActorTests.cpp
  core/RoomEventDispatcherTests.cpp
  core/RoomEventMetricsTests.cpp
  core/RoomEventQueueTests.cpp
  core/RoomEventTests.cpp
  core/RoomManagerTests.cpp
  core/WorkerPoolTests.cpp
  core/ServerRoomIntegrationTests.cpp
  core/TcpPacketTests.cpp
  core/TcpPacketReaderTests.cpp
  core/SessionManagerTests.cpp
  core/TcpListenerTests.cpp
  core/UdpSocketTests.cpp
  core/ServerIntegrationTests.cpp
  client/DebugCliCommandTests.cpp
  protocol/AckWindowTests.cpp
  protocol/RudpBattleStartPayloadTests.cpp
  protocol/RudpDatagramReceiverTests.cpp
  protocol/RudpGameEventPayloadTests.cpp
  protocol/RudpGameplayEventIdempotencyTrackerTests.cpp
  protocol/RudpHelloPayloadTests.cpp
  protocol/RudpInputCommandPayloadTests.cpp
  protocol/RudpInputCommandSequenceTrackerTests.cpp
  protocol/RudpMetaResponseIdempotencyTrackerTests.cpp
  protocol/RudpMetaResponsePayloadTests.cpp
  protocol/PacketParserTests.cpp
  protocol/PacketSerializerTests.cpp
  protocol/RudpPeerReceiveTests.cpp
  protocol/RudpPeerRegistryTests.cpp
  protocol/RudpReceivePipelineTests.cpp
  protocol/RudpReliableEventDeliveryGuardTests.cpp
  protocol/RudpReliableEventSendQueueTests.cpp
  protocol/RudpSocketDrainTests.cpp
  protocol/ReliableSendQueueTests.cpp
  protocol/RudpRetransmissionFlushTests.cpp
  protocol/RudpRetransmissionScanTests.cpp
  protocol/RudpSessionBinderTests.cpp
  protocol/DispatcherTests.cpp
)

target_compile_features(lol_tests PRIVATE cxx_std_17)
#add_subdirectory(../server server_build)

target_link_libraries(lol_tests PRIVATE GTest::gtest_main lol_server_core lol_debug_cli_core)
#target_include_directories(lol_tests PRIVATE ../server/src)

include(GoogleTest)
gtest_discover_tests(lol_tests)
