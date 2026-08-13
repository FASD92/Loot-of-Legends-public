add_executable(rudp_bind_tests
  RudpBindTests.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/AuthClaimCoordinator.cpp)
target_include_directories(rudp_bind_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root)
target_compile_definitions(rudp_bind_tests PRIVATE
  LOOT_RUDP_BIND_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/rudp-bind-v1.json")
target_link_libraries(rudp_bind_tests PRIVATE
  lol_meta_adapter
  lol_session
  lol_transport_rudp
  lol_transport_tcp)
loot_apply_compile_policy(rudp_bind_tests)
add_test(NAME rudp_bind COMMAND rudp_bind_tests)
