add_library(lol_transport_rudp STATIC
  src/ReliableQueue.cpp
  src/RudpBindingRegistry.cpp
  src/RudpCombatCodec.cpp
  src/RudpCodec.cpp
  src/RudpHeader.cpp
  src/RudpLootCodec.cpp
  src/RudpMovementCodec.cpp
  src/RudpPeer.cpp)

target_include_directories(lol_transport_rudp
  PUBLIC
    include)

loot_apply_compile_policy(lol_transport_rudp)
