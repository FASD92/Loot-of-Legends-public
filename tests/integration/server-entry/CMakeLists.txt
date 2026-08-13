add_executable(server_entry_tests
  ServerEntryTests.cpp)

target_compile_definitions(server_entry_tests PRIVATE
  LOOT_GAME_SERVER_PATH="$<TARGET_FILE:lol_game_server>")
target_link_libraries(server_entry_tests PRIVATE
  lol_transport_rudp
  lol_transport_tcp)
loot_apply_compile_policy(server_entry_tests)

add_dependencies(server_entry_tests lol_game_server)
add_test(NAME server_entry COMMAND server_entry_tests)
set_tests_properties(server_entry PROPERTIES TIMEOUT 90)
