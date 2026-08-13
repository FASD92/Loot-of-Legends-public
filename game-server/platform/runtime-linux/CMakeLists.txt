add_library(lol_runtime_linux STATIC
  src/EpollReactor.cpp
  src/UdpEndpoint.cpp)

target_include_directories(lol_runtime_linux
  PUBLIC
    include)

loot_apply_compile_policy(lol_runtime_linux)
