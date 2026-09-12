#pragma once

#include <chrono>
#include <cstdint>

namespace lol::transport::rudp {

struct RttEstimate final {
  std::uint64_t samples{0};
  std::chrono::microseconds srtt{0};
  std::chrono::microseconds rttvar{0};
  std::chrono::milliseconds rto{200};
};

class RttEstimator final {
public:
  static constexpr std::chrono::milliseconds kBootstrapRto{200};
  static constexpr std::chrono::milliseconds kMinimumRto{200};
  static constexpr std::chrono::milliseconds kMaximumRto{1000};
  static constexpr std::chrono::milliseconds kExpiry{5000};

  [[nodiscard]] bool observe(std::chrono::microseconds sample) noexcept;
  [[nodiscard]] RttEstimate snapshot() const noexcept;

private:
  RttEstimate estimate_;
};

} // namespace lol::transport::rudp
