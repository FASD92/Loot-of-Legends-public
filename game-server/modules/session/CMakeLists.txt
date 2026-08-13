add_library(lol_session STATIC
  src/SessionRegistry.cpp)

target_include_directories(lol_session
  PUBLIC
    include)
target_link_libraries(lol_session PUBLIC lol_shared_kernel)

loot_apply_compile_policy(lol_session)
