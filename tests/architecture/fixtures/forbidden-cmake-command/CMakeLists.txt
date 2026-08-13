cmake_minimum_required(VERSION 3.28)
project(ForbiddenCMakeCommand LANGUAGES CXX)

include_directories(include)
link_libraries(example)
file(GLOB_RECURSE SOURCES "*.cpp")
