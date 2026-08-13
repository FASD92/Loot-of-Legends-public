#include <lol/runtime/ProcessLifecycle.hpp>

namespace lol::runtime {

ProcessPhase ProcessLifecycle::phase() const noexcept { return phase_; }

bool ProcessLifecycle::start() noexcept {
  if (phase_ != ProcessPhase::Constructed) {
    return false;
  }
  phase_ = ProcessPhase::Running;
  return true;
}

bool ProcessLifecycle::requestStop() noexcept {
  if (phase_ != ProcessPhase::Running) {
    return false;
  }
  phase_ = ProcessPhase::StopRequested;
  return true;
}

bool ProcessLifecycle::markStopped() noexcept {
  if (phase_ != ProcessPhase::StopRequested) {
    return false;
  }
  phase_ = ProcessPhase::Stopped;
  return true;
}

} // namespace lol::runtime
