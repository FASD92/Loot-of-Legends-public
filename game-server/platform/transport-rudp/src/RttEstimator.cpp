#include <lol/transport/rudp/RttEstimator.hpp>

#include <algorithm>

namespace lol::transport::rudp {

bool RttEstimator::observe(std::chrono::microseconds sample) noexcept {
  if (sample <= std::chrono::microseconds::zero() || sample >= kExpiry) {
    return false;
  }
  if (estimate_.samples == 0) {
    estimate_.srtt = sample;
    estimate_.rttvar = sample / 2;
  } else {
    const auto difference = estimate_.srtt > sample ? estimate_.srtt - sample
                                                    : sample - estimate_.srtt;
    estimate_.rttvar = (estimate_.rttvar * 3 + difference) / 4;
    estimate_.srtt = (estimate_.srtt * 7 + sample) / 8;
  }
  ++estimate_.samples;
  const auto raw = estimate_.srtt + std::max(std::chrono::microseconds{1000},
                                             estimate_.rttvar * 4);
  estimate_.rto = std::clamp(std::chrono::ceil<std::chrono::milliseconds>(raw),
                             kMinimumRto, kMaximumRto);
  return true;
}

RttEstimate RttEstimator::snapshot() const noexcept { return estimate_; }

} // namespace lol::transport::rudp
