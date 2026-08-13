add_executable(rematch_tests
  RematchTests.cpp)
target_link_libraries(rematch_tests PRIVATE
  lol_game_flow)
loot_apply_compile_policy(rematch_tests)
add_test(NAME rematch COMMAND rematch_tests)
