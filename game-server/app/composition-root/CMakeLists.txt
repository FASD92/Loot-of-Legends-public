add_executable(lol_game_server
  main.cpp
  AuthClaimCoordinator.cpp
  BattleLoadFlow.cpp
  ConfiguredGameServer.cpp
  LobbyRoomFlow.cpp
  RudpCombatFlow.cpp
  RudpMovementFlow.cpp
  SessionAuthFlow.cpp)

loot_configure_build_identity(
  "${CMAKE_CURRENT_SOURCE_DIR}/BuildIdentity.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildIdentity.hpp")

target_include_directories(lol_game_server PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(lol_game_server PRIVATE
  lol_game_flow
  lol_meta_adapter
  lol_observability
  lol_runtime
  lol_runtime_linux
  lol_session
  lol_settlement
  lol_settlement_storage
  lol_transport_rudp
  lol_transport_tcp)
loot_apply_compile_policy(lol_game_server)

if(BUILD_TESTING)
  target_compile_definitions(lol_game_server PRIVATE
    LOOT_ENABLE_TEST_META_FIXTURE=1)

  add_test(NAME game_server_version COMMAND lol_game_server --version)
  set_tests_properties(game_server_version PROPERTIES
    PASS_REGULAR_EXPRESSION
      "\"product\":\"loot-of-legends-v2-game-server\".*\"productVersion\":\"0.0.0-public\".*\"sourceRevision\":\"[0-9a-f]+\".*\"sourceDirty\":(true|false).*\"cxxStandard\":20")

  add_test(NAME game_server_smoke COMMAND lol_game_server --smoke-run)
  set_tests_properties(game_server_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "Constructed -> Running -> StopRequested -> Stopped")

  add_test(NAME game_server_unknown_argument COMMAND lol_game_server --unknown)
  set_tests_properties(game_server_unknown_argument PROPERTIES WILL_FAIL TRUE)
endif()
