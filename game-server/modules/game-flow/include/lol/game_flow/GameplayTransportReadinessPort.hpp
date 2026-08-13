#pragma once

#include <lol/shared/Identifiers.hpp>

namespace lol::game_flow {

class GameplayTransportReadinessPort {
public:
  virtual ~GameplayTransportReadinessPort() = default;

  [[nodiscard]] virtual bool
  isReady(shared::SessionId sessionId,
          shared::SessionGeneration generation) const noexcept = 0;
};

} // namespace lol::game_flow
