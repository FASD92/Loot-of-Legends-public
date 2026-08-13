find_package(Threads REQUIRED)

add_library(lol_settlement_storage STATIC
  src/JournalCodec.cpp
  src/JournalRecovery.cpp
  src/SegmentJournal.cpp
  src/StorageProbe.cpp
  src/StorageWorker.cpp)

target_include_directories(lol_settlement_storage
  PUBLIC
    include)
target_link_libraries(lol_settlement_storage PUBLIC
  lol_settlement
  Threads::Threads)
loot_apply_compile_policy(lol_settlement_storage)
