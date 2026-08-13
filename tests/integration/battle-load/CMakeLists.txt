add_executable(battle_load_ordering_tests
  LoadOrderingTests.cpp)
target_include_directories(battle_load_ordering_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/modules/game-flow/src)
target_link_libraries(battle_load_ordering_tests PRIVATE lol_game_flow)
loot_apply_compile_policy(battle_load_ordering_tests)
add_test(NAME battle_load_ordering COMMAND battle_load_ordering_tests)

add_executable(battle_load_protocol_tests
  BattleLoadProtocolTests.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/BattleLoadFlow.cpp)
target_include_directories(battle_load_protocol_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root)
target_compile_definitions(battle_load_protocol_tests PRIVATE
  LOOT_BATTLE_LOAD_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/battle-load-v1.json")
target_link_libraries(battle_load_protocol_tests PRIVATE
  lol_game_flow
  lol_transport_tcp)
loot_apply_compile_policy(battle_load_protocol_tests)
add_test(NAME battle_load_protocol COMMAND battle_load_protocol_tests)
