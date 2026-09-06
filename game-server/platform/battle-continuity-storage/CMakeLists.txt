find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(lol_battle_continuity_storage STATIC
  src/ContinuityStorage.cpp)

target_include_directories(lol_battle_continuity_storage
  PUBLIC
    include)
target_link_libraries(lol_battle_continuity_storage
  PUBLIC
    lol_battle_continuity
  PRIVATE
    OpenSSL::Crypto
    Threads::Threads)

loot_apply_compile_policy(lol_battle_continuity_storage)

if(BUILD_TESTING)
  add_executable(battle_continuity_storage_tests
    tests/ContinuityStorageTests.cpp)
  target_link_libraries(battle_continuity_storage_tests PRIVATE
    lol_battle_continuity_storage)
  loot_apply_compile_policy(battle_continuity_storage_tests)
  add_test(NAME battle_continuity_storage COMMAND battle_continuity_storage_tests)
endif()
