if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(lol_debug_cli LANGUAGES CXX)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

add_library(
  lol_debug_cli_core
  debug_cli/DebugCli.cpp
  debug_cli/DebugCli.hpp
  debug_cli/DebugCliCommand.cpp
  debug_cli/DebugCliCommand.hpp
  debug_cli/DebugClientTransport.cpp
  debug_cli/DebugClientTransport.hpp
)

if(NOT TARGET lol_server_core)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../server ${CMAKE_CURRENT_BINARY_DIR}/server_build)
endif()

target_include_directories(lol_debug_cli_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lol_debug_cli_core PUBLIC cxx_std_17)
target_link_libraries(lol_debug_cli_core PUBLIC lol_server_core)

add_executable(lol_debug_cli debug_cli/main.cpp)
target_link_libraries(lol_debug_cli PRIVATE lol_debug_cli_core)
