function(loot_apply_compile_policy target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wformat=2
      -Wundef
      -Wnon-virtual-dtor
      -Wold-style-cast)

    if(LOOT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(LOOT_SANITIZERS)
    target_compile_options(${target} PRIVATE
      -fsanitize=${LOOT_SANITIZERS}
      -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE
      -fsanitize=${LOOT_SANITIZERS}
      -fno-omit-frame-pointer)
  endif()
endfunction()
