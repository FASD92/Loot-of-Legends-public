cmake_minimum_required(VERSION 3.28)

if(CMAKE_HOST_APPLE AND CMAKE_CXX_COMPILER MATCHES "g\\+\\+-13$")
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE LOOT_MACOS_SDK
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  set(CMAKE_OSX_SYSROOT "${LOOT_MACOS_SDK}" CACHE PATH "macOS SDK" FORCE)
endif()

project(LootOfLegendsV2 VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(DEFINED LOOT_REQUIRED_COMPILER_ID
   AND NOT CMAKE_CXX_COMPILER_ID STREQUAL LOOT_REQUIRED_COMPILER_ID)
  message(FATAL_ERROR
    "Expected ${LOOT_REQUIRED_COMPILER_ID}, got ${CMAKE_CXX_COMPILER_ID}")
endif()

if(DEFINED LOOT_REQUIRED_COMPILER_MAJOR
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS LOOT_REQUIRED_COMPILER_MAJOR)
  message(FATAL_ERROR
    "Expected compiler >= ${LOOT_REQUIRED_COMPILER_MAJOR}, got ${CMAKE_CXX_COMPILER_VERSION}")
endif()

include(CTest)
include(cmake/LootCompilerWarnings.cmake)
include(cmake/LootBuildIdentity.cmake)

add_subdirectory(game-server/shared-kernel)
add_subdirectory(game-server/modules/session)
add_subdirectory(game-server/modules/lobby-room)
add_subdirectory(game-server/modules/battle)
add_subdirectory(game-server/modules/settlement)
add_subdirectory(game-server/platform/settlement-storage)
add_subdirectory(game-server/platform/runtime)
add_subdirectory(game-server/platform/runtime-linux)
add_subdirectory(game-server/modules/game-flow)
add_subdirectory(game-server/platform/transport-tcp)
add_subdirectory(game-server/platform/transport-rudp)
add_subdirectory(game-server/platform/meta-adapter)
add_subdirectory(game-server/platform/observability)
add_subdirectory(game-server/app/composition-root)

if(BUILD_TESTING)
  add_subdirectory(tests/runtime-linux)
  add_subdirectory(tests/integration/session)
  add_subdirectory(tests/integration/lobby-room)
  add_subdirectory(tests/integration/battle-load)
  add_subdirectory(tests/integration/rudp-bind)
  add_subdirectory(tests/integration/movement)
  add_subdirectory(tests/integration/combat)
  add_subdirectory(tests/integration/settlement)
  add_subdirectory(tests/integration/rematch)
  add_subdirectory(tests/integration/server-entry)
  add_subdirectory(tests/settlement-storage)
  add_subdirectory(tests/transport-rudp)
endif()
