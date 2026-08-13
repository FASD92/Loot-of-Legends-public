add_library(lol_settlement STATIC
  src/application/SettlementCapacityGate.cpp
  src/application/SettlementPublisher.cpp
  src/domain/SettlementIntent.cpp)

target_include_directories(lol_settlement
  PUBLIC
    include)
target_link_libraries(lol_settlement PUBLIC lol_shared_kernel)

loot_apply_compile_policy(lol_settlement)

if(BUILD_TESTING)
  add_executable(settlement_intent_tests
    tests/SettlementIntentTests.cpp)
  target_link_libraries(settlement_intent_tests PRIVATE lol_settlement)
  loot_apply_compile_policy(settlement_intent_tests)
  add_test(NAME settlement_intent COMMAND settlement_intent_tests)
endif()
