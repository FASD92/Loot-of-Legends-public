#pragma once

#include <filesystem>

namespace lol::settlement_storage {

enum class StorageProbeResult {
  Ready,
  MissingDirectory,
  UnsafePath,
  InsecurePermissions,
  IoFailure,
};

StorageProbeResult probeJournalDirectory(const std::filesystem::path &path);

} // namespace lol::settlement_storage
