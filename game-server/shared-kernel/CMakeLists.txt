add_library(lol_shared_kernel STATIC
  src/Identifiers.cpp)

target_include_directories(lol_shared_kernel
  PUBLIC
    include)

loot_apply_compile_policy(lol_shared_kernel)
