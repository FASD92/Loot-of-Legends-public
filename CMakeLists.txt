cmake_minimum_required(VERSION 3.20)
project(LootOfLegends LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(server)
add_subdirectory(client)
add_subdirectory(tests)
