#include <lol/runtime/ProcessLifecycle.hpp>

#include <cstdlib>

int main() {
  lol::runtime::ProcessLifecycle lifecycle;

  if (lifecycle.phase() != lol::runtime::ProcessPhase::Constructed) {
    return EXIT_FAILURE;
  }
  if (lifecycle.requestStop() || lifecycle.markStopped()) {
    return EXIT_FAILURE;
  }
  if (!lifecycle.start() || lifecycle.start()) {
    return EXIT_FAILURE;
  }
  if (lifecycle.phase() != lol::runtime::ProcessPhase::Running ||
      lifecycle.markStopped()) {
    return EXIT_FAILURE;
  }
  if (!lifecycle.requestStop() || lifecycle.requestStop() ||
      lifecycle.start()) {
    return EXIT_FAILURE;
  }
  if (!lifecycle.markStopped() || lifecycle.markStopped()) {
    return EXIT_FAILURE;
  }
  if (lifecycle.phase() != lol::runtime::ProcessPhase::Stopped ||
      lifecycle.start() || lifecycle.requestStop()) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
