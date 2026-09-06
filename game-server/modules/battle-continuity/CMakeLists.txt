find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(lol_battle_continuity STATIC
  src/RecordCodec.cpp
  src/BattleStateCodec.cpp
  src/RecoveryStateCodec.cpp
  src/FlightRecorder.cpp
  src/BattleReplay.cpp
  src/RecoveryEnvelopeCodec.cpp)

target_include_directories(lol_battle_continuity
  PUBLIC
    include)
target_link_libraries(lol_battle_continuity
  PUBLIC
    lol_shared_kernel
    lol_battle
    OpenSSL::Crypto)

loot_apply_compile_policy(lol_battle_continuity)

if(BUILD_TESTING)
  add_executable(battle_continuity_codec_tests
    tests/RecordCodecTests.cpp)
  target_include_directories(battle_continuity_codec_tests PRIVATE include)
  target_link_libraries(battle_continuity_codec_tests PRIVATE
    lol_battle_continuity)
  loot_apply_compile_policy(battle_continuity_codec_tests)
  add_test(NAME battle_continuity_codec COMMAND battle_continuity_codec_tests)

  add_executable(battle_state_codec_tests
    tests/BattleStateCodecTests.cpp)
  target_link_libraries(battle_state_codec_tests PRIVATE
    lol_battle_continuity)
  loot_apply_compile_policy(battle_state_codec_tests)
  add_test(NAME battle_state_codec COMMAND battle_state_codec_tests)

  add_executable(battle_recovery_state_codec_tests
    tests/RecoveryStateCodecTests.cpp)
  target_link_libraries(battle_recovery_state_codec_tests PRIVATE
    lol_battle_continuity)
  loot_apply_compile_policy(battle_recovery_state_codec_tests)
  add_test(NAME battle_recovery_state_codec
    COMMAND battle_recovery_state_codec_tests)

  add_executable(battle_flight_recorder_tests
    tests/FlightRecorderTests.cpp)
  target_link_libraries(battle_flight_recorder_tests PRIVATE
    lol_battle_continuity)
  loot_apply_compile_policy(battle_flight_recorder_tests)
  add_test(NAME battle_flight_recorder COMMAND battle_flight_recorder_tests)

  add_executable(battle_replay_tests
    tests/BattleReplayTests.cpp)
  target_link_libraries(battle_replay_tests PRIVATE
    lol_battle_continuity)
  loot_apply_compile_policy(battle_replay_tests)
  add_test(NAME battle_replay COMMAND battle_replay_tests)

  add_executable(battle_recovery_envelope_codec_tests
    tests/RecoveryEnvelopeCodecTests.cpp)
  target_link_libraries(battle_recovery_envelope_codec_tests PRIVATE
    lol_battle_continuity)
  loot_apply_compile_policy(battle_recovery_envelope_codec_tests)
  add_test(NAME battle_recovery_envelope_codec
    COMMAND battle_recovery_envelope_codec_tests)
endif()

add_executable(lol_battle_replay
  ../../tools/battle-replay/main.cpp)
target_link_libraries(lol_battle_replay PRIVATE lol_battle_continuity)
loot_apply_compile_policy(lol_battle_replay)
