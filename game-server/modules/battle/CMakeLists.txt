add_library(lol_battle STATIC
  src/application/AttackResultStore.cpp
  src/application/LootHandler.cpp
  src/application/LootResultStore.cpp
  src/application/ResultBuilder.cpp
  src/domain/BattleInstance.cpp
  src/domain/CombatState.cpp
  src/domain/Drop.cpp
  src/domain/RelicCatalog.cpp)

target_include_directories(lol_battle
  PUBLIC
    include)
target_link_libraries(lol_battle PUBLIC lol_shared_kernel)

loot_apply_compile_policy(lol_battle)

if(BUILD_TESTING)
  add_executable(battle_load_tests
    tests/BattleLoadTests.cpp)
  target_link_libraries(battle_load_tests PRIVATE lol_battle)
  loot_apply_compile_policy(battle_load_tests)
  add_test(NAME battle_load COMMAND battle_load_tests)

  add_executable(battle_combat_domain_tests
    tests/CombatDomainTests.cpp)
  target_link_libraries(battle_combat_domain_tests PRIVATE lol_battle)
  loot_apply_compile_policy(battle_combat_domain_tests)
  add_test(NAME battle_combat_domain COMMAND battle_combat_domain_tests)

  add_executable(battle_attack_result_store_tests
    tests/AttackResultStoreTests.cpp)
  target_link_libraries(battle_attack_result_store_tests PRIVATE lol_battle)
  loot_apply_compile_policy(battle_attack_result_store_tests)
  add_test(NAME battle_attack_result_store COMMAND battle_attack_result_store_tests)

  add_executable(battle_loot_domain_tests
    tests/LootDomainTests.cpp)
  target_link_libraries(battle_loot_domain_tests PRIVATE lol_battle)
  loot_apply_compile_policy(battle_loot_domain_tests)
  add_test(NAME battle_loot_domain COMMAND battle_loot_domain_tests)

  add_executable(battle_loot_claim_tests
    tests/LootClaimTests.cpp)
  target_link_libraries(battle_loot_claim_tests PRIVATE lol_battle)
  loot_apply_compile_policy(battle_loot_claim_tests)
  add_test(NAME battle_loot_claim COMMAND battle_loot_claim_tests)

  add_executable(battle_final_result_tests
    tests/BattleResultTests.cpp)
  target_link_libraries(battle_final_result_tests PRIVATE lol_battle)
  loot_apply_compile_policy(battle_final_result_tests)
  add_test(NAME battle_final_result COMMAND battle_final_result_tests)
endif()
