#pragma once

namespace lol::runtime {

enum class ProcessPhase { Constructed, Running, StopRequested, Stopped };

class ProcessLifecycle final {
public:
  [[nodiscard]] ProcessPhase phase() const noexcept;
  [[nodiscard]] bool start() noexcept;
  [[nodiscard]] bool requestStop() noexcept;
  [[nodiscard]] bool markStopped() noexcept;

private:
  ProcessPhase phase_{ProcessPhase::Constructed};
};

} // namespace lol::runtime
