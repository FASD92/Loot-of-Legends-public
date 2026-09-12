add_executable(server_entry_tests
  ServerEntryTests.cpp)

target_compile_definitions(server_entry_tests PRIVATE
  LOOT_GAME_SERVER_PATH="$<TARGET_FILE:lol_game_server>")
target_link_libraries(server_entry_tests PRIVATE
  lol_battle_continuity
  lol_battle_continuity_storage
  lol_settlement_storage
  lol_transport_rudp
  lol_transport_tcp)
loot_apply_compile_policy(server_entry_tests)

add_dependencies(server_entry_tests lol_game_server)
add_test(NAME server_entry COMMAND server_entry_tests)
set_tests_properties(server_entry PROPERTIES TIMEOUT 90)
add_test(NAME server_entry_reconnect COMMAND server_entry_tests reconnect)
set_tests_properties(server_entry_reconnect PROPERTIES TIMEOUT 90)
add_test(NAME server_entry_continuity COMMAND server_entry_tests continuity)
set_tests_properties(server_entry_continuity PROPERTIES TIMEOUT 120)
add_test(NAME server_entry_terminal_retirement
         COMMAND server_entry_tests terminal-retirement)
set_tests_properties(server_entry_terminal_retirement PROPERTIES TIMEOUT 100)
add_test(NAME server_entry_movement_ack COMMAND server_entry_tests movement-ack)
set_tests_properties(server_entry_movement_ack PROPERTIES TIMEOUT 30)
add_test(NAME server_entry_ack_only COMMAND server_entry_tests ack-only)
set_tests_properties(server_entry_ack_only PROPERTIES TIMEOUT 30)
add_test(NAME server_entry_rtt_sampling COMMAND server_entry_tests rtt-sampling)
set_tests_properties(server_entry_rtt_sampling PROPERTIES TIMEOUT 30)
