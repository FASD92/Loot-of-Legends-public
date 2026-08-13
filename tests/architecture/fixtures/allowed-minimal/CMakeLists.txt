cmake_minimum_required(VERSION 3.28)
project(AllowedMinimal LANGUAGES CXX)

add_library(lol_runtime STATIC runtime.cpp)
add_executable(lol_game_server main.cpp)
target_link_libraries(lol_game_server PRIVATE lol_runtime)
