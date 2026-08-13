set(LOOT_PRODUCT_VERSION "0.0.0-public")

function(loot_configure_build_identity template output)
  if(DEFINED ENV{GITHUB_SHA} AND NOT "$ENV{GITHUB_SHA}" STREQUAL "")
    set(source_revision "$ENV{GITHUB_SHA}")
  else()
    execute_process(
      COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
      RESULT_VARIABLE revision_result
      OUTPUT_VARIABLE source_revision
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT revision_result EQUAL 0)
      message(FATAL_ERROR "Unable to resolve source revision")
    endif()
  endif()

  string(LENGTH "${source_revision}" revision_length)
  if(NOT revision_length EQUAL 40 OR NOT source_revision MATCHES "^[0-9A-Fa-f]+$")
    message(FATAL_ERROR "Source revision must be a 40-character hexadecimal SHA")
  endif()
  string(TOLOWER "${source_revision}" LOOT_SOURCE_REVISION)

  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" status --porcelain
    RESULT_VARIABLE dirty_result
    OUTPUT_VARIABLE dirty_output
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT dirty_result EQUAL 0)
    message(FATAL_ERROR "Unable to determine source dirty state")
  endif()

  if(dirty_output STREQUAL "")
    set(LOOT_SOURCE_DIRTY false)
  else()
    set(LOOT_SOURCE_DIRTY true)
  endif()

  if(LOOT_REQUIRE_CLEAN_SOURCE AND LOOT_SOURCE_DIRTY)
    message(FATAL_ERROR "release-evidence requires a clean source tree")
  endif()

  set(LOOT_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
  set(LOOT_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
  set(LOOT_CMAKE_VERSION "${CMAKE_VERSION}")

  get_filename_component(output_directory "${output}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_directory}")
  configure_file("${template}" "${output}" @ONLY)
endfunction()
