add_executable(rudp_header_tests
  RudpHeaderTests.cpp)
target_compile_definitions(rudp_header_tests PRIVATE
  LOOT_RUDP_BIND_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/rudp-bind-v1.json"
  LOOT_MOVEMENT_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/contracts/protocol/golden/movement-v1.json")
target_link_libraries(rudp_header_tests PRIVATE lol_transport_rudp)
loot_apply_compile_policy(rudp_header_tests)
add_test(NAME rudp_header COMMAND rudp_header_tests)

add_executable(rudp_delivery_tests
  RudpDeliveryTests.cpp)
target_link_libraries(rudp_delivery_tests PRIVATE lol_transport_rudp)
loot_apply_compile_policy(rudp_delivery_tests)
add_test(NAME rudp_delivery COMMAND rudp_delivery_tests)
