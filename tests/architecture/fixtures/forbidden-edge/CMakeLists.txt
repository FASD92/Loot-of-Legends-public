cmake_minimum_required(VERSION 3.28)
project(ForbiddenEdge LANGUAGES CXX)

add_library(lol_settlement STATIC settlement.cpp)
add_library(lol_battle STATIC battle.cpp)
target_link_libraries(lol_battle PRIVATE lol_settlement)
