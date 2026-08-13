add_library(lol_runtime STATIC
  src/DeadlineScheduler.cpp
  src/ProcessLifecycle.cpp
  src/WorkerPool.cpp)

target_include_directories(lol_runtime
  PUBLIC
    include)
find_package(Threads REQUIRED)
target_link_libraries(lol_runtime PUBLIC Threads::Threads)

loot_apply_compile_policy(lol_runtime)

if(BUILD_TESTING)
  add_executable(process_lifecycle_tests
    tests/ProcessLifecycleTests.cpp)
  target_link_libraries(process_lifecycle_tests PRIVATE lol_runtime)
  loot_apply_compile_policy(process_lifecycle_tests)
  add_test(NAME process_lifecycle COMMAND process_lifecycle_tests)

  add_executable(deadline_scheduler_tests
    tests/DeadlineSchedulerTests.cpp)
  target_link_libraries(deadline_scheduler_tests PRIVATE lol_runtime)
  loot_apply_compile_policy(deadline_scheduler_tests)
  add_test(NAME deadline_scheduler COMMAND deadline_scheduler_tests)

  add_executable(worker_pool_tests
    tests/WorkerPoolTests.cpp)
  target_link_libraries(worker_pool_tests PRIVATE lol_runtime)
  loot_apply_compile_policy(worker_pool_tests)
  add_test(NAME worker_pool COMMAND worker_pool_tests)
endif()
