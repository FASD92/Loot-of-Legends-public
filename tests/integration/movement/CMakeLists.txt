add_executable(movement_tests
  MovementTests.cpp)
target_link_libraries(movement_tests PRIVATE
  lol_battle)
loot_apply_compile_policy(movement_tests)
add_test(NAME movement COMMAND movement_tests)

add_executable(movement_snapshot_tests
  MovementSnapshotTests.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/RudpMovementFlow.cpp)
target_include_directories(movement_snapshot_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root)
target_compile_definitions(movement_snapshot_tests PRIVATE
  LOOT_MOVEMENT_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/movement-v1.json")
target_link_libraries(movement_snapshot_tests PRIVATE
  lol_game_flow
  lol_transport_rudp)
loot_apply_compile_policy(movement_snapshot_tests)
add_test(NAME movement_snapshot COMMAND movement_snapshot_tests)
