add_library(lol_game_flow STATIC
  src/RoomCommandGateway.cpp
  src/execution/RoomExecutionCell.cpp
  src/execution/RoomExecutionDirectory.cpp
  src/execution/SessionRouteIndex.cpp
  src/workflows/BattleRecoveryWorkflow.cpp
  src/workflows/BattleTerminalWorkflow.cpp
  src/workflows/CombatTickWorkflow.cpp
  src/workflows/DurabilityCompletionWorkflow.cpp
  src/workflows/GameplayTickWorkflow.cpp
  src/workflows/HostStartWorkflow.cpp
  src/workflows/LoadBarrierWorkflow.cpp)

target_include_directories(lol_game_flow
  PUBLIC include
  PRIVATE src)
target_link_libraries(lol_game_flow PUBLIC
  lol_lobby_room
  lol_battle
  lol_settlement
  lol_runtime)

loot_apply_compile_policy(lol_game_flow)

if(BUILD_TESTING)
  add_executable(room_execution_cell_tests
    tests/RoomExecutionCellTests.cpp)
  target_include_directories(room_execution_cell_tests PRIVATE src)
  target_link_libraries(room_execution_cell_tests PRIVATE lol_game_flow)
  loot_apply_compile_policy(room_execution_cell_tests)
  add_test(NAME room_execution_cell COMMAND room_execution_cell_tests)

  add_executable(room_routing_tests
    tests/RoomRoutingTests.cpp)
  target_include_directories(room_routing_tests PRIVATE src)
  target_link_libraries(room_routing_tests PRIVATE lol_game_flow)
  loot_apply_compile_policy(room_routing_tests)
  add_test(NAME room_routing COMMAND room_routing_tests)

  add_executable(battle_recovery_workflow_tests
    tests/BattleRecoveryWorkflowTests.cpp)
  target_include_directories(battle_recovery_workflow_tests PRIVATE src)
  target_link_libraries(battle_recovery_workflow_tests PRIVATE lol_game_flow)
  loot_apply_compile_policy(battle_recovery_workflow_tests)
  add_test(NAME battle_recovery_workflow
    COMMAND battle_recovery_workflow_tests)
endif()
