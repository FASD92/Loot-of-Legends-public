find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(lol_observability STATIC
  src/GameMetrics.cpp
  src/PrivateMetricsServer.cpp)
target_include_directories(lol_observability PUBLIC include)
target_link_libraries(lol_observability
  PUBLIC
    lol_game_flow
    lol_settlement
  PRIVATE
    OpenSSL::Crypto
    Threads::Threads)
loot_apply_compile_policy(lol_observability)

if(BUILD_TESTING)
  add_executable(private_metrics_server_tests
    tests/PrivateMetricsServerTests.cpp)
  target_link_libraries(private_metrics_server_tests PRIVATE
    lol_observability)
  loot_apply_compile_policy(private_metrics_server_tests)
  add_test(NAME private_metrics_server COMMAND private_metrics_server_tests)
  set_tests_properties(private_metrics_server PROPERTIES TIMEOUT 10)
endif()
