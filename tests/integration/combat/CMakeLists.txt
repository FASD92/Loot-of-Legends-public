add_executable(combat_outcome_tests
  CombatOutcomeTests.cpp)
target_include_directories(combat_outcome_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/modules/game-flow/src)
target_link_libraries(combat_outcome_tests PRIVATE
  lol_game_flow)
loot_apply_compile_policy(combat_outcome_tests)
add_test(NAME combat_outcome COMMAND combat_outcome_tests)

add_executable(combat_protocol_tests
  CombatProtocolTests.cpp)
target_compile_definitions(combat_protocol_tests PRIVATE
  LOOT_COMBAT_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/combat-v1.json")
target_link_libraries(combat_protocol_tests PRIVATE
  lol_battle
  lol_transport_rudp)
loot_apply_compile_policy(combat_protocol_tests)
add_test(NAME combat_protocol COMMAND combat_protocol_tests)

add_executable(loot_protocol_tests
  LootProtocolTests.cpp)
target_compile_definitions(loot_protocol_tests PRIVATE
  LOOT_PROTOCOL_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/loot-v1.json")
target_link_libraries(loot_protocol_tests PRIVATE
  lol_transport_rudp)
loot_apply_compile_policy(loot_protocol_tests)
add_test(NAME loot_protocol COMMAND loot_protocol_tests)

add_executable(combat_flow_tests
  CombatFlowTests.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/AuthClaimCoordinator.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/RudpCombatFlow.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/RudpMovementFlow.cpp)
target_include_directories(combat_flow_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root)
target_link_libraries(combat_flow_tests PRIVATE
  lol_game_flow
  lol_meta_adapter
  lol_session
  lol_transport_rudp)
loot_apply_compile_policy(combat_flow_tests)
add_test(NAME combat_flow COMMAND combat_flow_tests)
