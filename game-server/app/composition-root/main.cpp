#include "BuildIdentity.hpp"
#include "ConfiguredGameServer.hpp"

#include <lol/runtime/ProcessLifecycle.hpp>

#include <iostream>
#include <string_view>

namespace {

constexpr int kUsageError = 2;

void printUsage() {
  std::cerr
      << "Usage: lol_game_server [--version|--smoke-run|--config <path>]\n";
}

int printVersion() {
  std::cout << "{\"product\":\"" << lol::build::kProduct
            << "\",\"productVersion\":\"" << lol::build::kProductVersion
            << "\",\"sourceRevision\":\"" << lol::build::kSourceRevision
            << "\",\"sourceDirty\":"
            << (lol::build::kSourceDirty ? "true" : "false")
            << ",\"cxxStandard\":" << lol::build::kCxxStandard
            << ",\"compilerId\":\"" << lol::build::kCompilerId
            << "\",\"compilerVersion\":\"" << lol::build::kCompilerVersion
            << "\",\"cmakeVersion\":\"" << lol::build::kCMakeVersion << "\"}\n";
  return 0;
}

int runSmoke() {
  lol::runtime::ProcessLifecycle lifecycle;
  if (lifecycle.phase() != lol::runtime::ProcessPhase::Constructed ||
      !lifecycle.start() || !lifecycle.requestStop() ||
      !lifecycle.markStopped() ||
      lifecycle.phase() != lol::runtime::ProcessPhase::Stopped) {
    return 1;
  }

  std::cout << "Constructed -> Running -> StopRequested -> Stopped\n";
  return 0;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc == 3 && std::string_view{argv[1]} == "--config") {
    return lol::app::runConfiguredGameServer(argv[2]);
  }
  if (argc != 2) {
    printUsage();
    return kUsageError;
  }

  const std::string_view argument{argv[1]};
  if (argument == "--version") {
    return printVersion();
  }
  if (argument == "--smoke-run") {
    return runSmoke();
  }

  printUsage();
  return kUsageError;
}
