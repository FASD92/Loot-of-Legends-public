add_library(lol_transport_tcp STATIC
  src/BattleLoadProtocol.cpp
  src/BattleRecoveryProtocol.cpp
  src/FinalResultCodec.cpp
  src/LobbyRoomProtocol.cpp
  src/SessionProtocolCodec.cpp
  src/TcpConnection.cpp)

target_include_directories(lol_transport_tcp
  PUBLIC
    include)

loot_apply_compile_policy(lol_transport_tcp)

if(BUILD_TESTING)
  add_executable(tcp_battle_recovery_protocol_tests
    tests/BattleRecoveryProtocolTests.cpp)
  target_link_libraries(tcp_battle_recovery_protocol_tests PRIVATE
    lol_transport_tcp)
  loot_apply_compile_policy(tcp_battle_recovery_protocol_tests)
  add_test(NAME tcp_battle_recovery_protocol
    COMMAND tcp_battle_recovery_protocol_tests)
endif()
