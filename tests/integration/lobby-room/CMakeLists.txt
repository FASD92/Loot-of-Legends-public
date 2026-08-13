add_executable(lobby_room_protocol_tests
  LobbyRoomProtocolTests.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/LobbyRoomFlow.cpp)

target_include_directories(lobby_room_protocol_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root)
target_compile_definitions(lobby_room_protocol_tests PRIVATE
  LOOT_LOBBY_ROOM_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/lobby-room-v1.json"
  LOOT_FINAL_RESULT_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/final-result-v1.json")
target_link_libraries(lobby_room_protocol_tests PRIVATE
  lol_game_flow
  lol_transport_tcp)
loot_apply_compile_policy(lobby_room_protocol_tests)

add_test(NAME lobby_room_protocol COMMAND lobby_room_protocol_tests)

add_executable(room_gateway_race_tests
  RoomGatewayRaceTests.cpp)
target_link_libraries(room_gateway_race_tests PRIVATE
  lol_game_flow)
loot_apply_compile_policy(room_gateway_race_tests)

add_test(NAME room_gateway_races COMMAND room_gateway_race_tests)
