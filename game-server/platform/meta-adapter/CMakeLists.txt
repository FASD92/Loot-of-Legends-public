find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(lol_meta_adapter STATIC
  src/CurlHttpsExchange.cpp
  src/MetaClaimClient.cpp
  src/MetaSettlementClient.cpp)

target_include_directories(lol_meta_adapter
  PUBLIC
    include)
target_link_libraries(lol_meta_adapter
  PUBLIC
    lol_settlement
  PRIVATE
    CURL::libcurl
    OpenSSL::Crypto
    Threads::Threads)

loot_apply_compile_policy(lol_meta_adapter)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(curl_https_probe tests/CurlHttpsProbe.cpp)
  target_link_libraries(curl_https_probe PRIVATE lol_meta_adapter)
  loot_apply_compile_policy(curl_https_probe)
  add_test(
    NAME curl_https_exchange
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_curl_https.py
      $<TARGET_FILE:curl_https_probe>)
  set_tests_properties(curl_https_exchange PROPERTIES TIMEOUT 30)
endif()
