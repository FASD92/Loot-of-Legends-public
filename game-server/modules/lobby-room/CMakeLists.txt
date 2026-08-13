add_library(lol_lobby_room STATIC
  src/domain/Room.cpp)

target_include_directories(lol_lobby_room
  PUBLIC
    include)
target_link_libraries(lol_lobby_room PUBLIC lol_shared_kernel)

loot_apply_compile_policy(lol_lobby_room)

if(BUILD_TESTING)
  add_executable(lobby_room_domain_tests
    tests/RoomDomainTests.cpp)
  target_link_libraries(lobby_room_domain_tests PRIVATE lol_lobby_room)
  loot_apply_compile_policy(lobby_room_domain_tests)
  add_test(NAME lobby_room_domain COMMAND lobby_room_domain_tests)
endif()
