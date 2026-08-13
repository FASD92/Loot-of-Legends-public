add_executable(session_registry_tests
  SessionRegistryTests.cpp)

target_link_libraries(session_registry_tests PRIVATE lol_session)
loot_apply_compile_policy(session_registry_tests)

add_test(NAME session_registry COMMAND session_registry_tests)

add_executable(tcp_transport_tests
  TcpTransportTests.cpp)

target_link_libraries(tcp_transport_tests PRIVATE
  lol_runtime_linux
  lol_transport_tcp)
loot_apply_compile_policy(tcp_transport_tests)

add_test(NAME tcp_transport COMMAND tcp_transport_tests)

add_executable(meta_claim_adapter_tests
  MetaClaimAdapterTests.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/AuthClaimCoordinator.cpp)

target_include_directories(meta_claim_adapter_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root)
target_link_libraries(meta_claim_adapter_tests PRIVATE
  lol_meta_adapter
  lol_session
  lol_transport_rudp)
loot_apply_compile_policy(meta_claim_adapter_tests)

add_test(NAME meta_claim_adapter COMMAND meta_claim_adapter_tests)

add_executable(session_authentication_tests
  SessionAuthenticationTests.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/AuthClaimCoordinator.cpp
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root/SessionAuthFlow.cpp)

target_include_directories(session_authentication_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/game-server/app/composition-root)
target_link_libraries(session_authentication_tests PRIVATE
  lol_meta_adapter
  lol_session
  lol_transport_rudp
  lol_transport_tcp)
loot_apply_compile_policy(session_authentication_tests)

add_test(NAME session_authentication COMMAND session_authentication_tests)
