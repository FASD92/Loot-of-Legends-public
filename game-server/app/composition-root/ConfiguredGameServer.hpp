#pragma once

#include <filesystem>

namespace lol::app {

// Loads and validates the operational config before exposing ingress, then
// owns the complete configured process lifetime until SIGINT or SIGTERM.
[[nodiscard]] int
runConfiguredGameServer(const std::filesystem::path &configPath);

} // namespace lol::app
