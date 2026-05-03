#include "Util/Time.hpp"

namespace Util {
TimePoint now() {
    return Clock::now();
}

uint64_t toMillis(TimePoint timePoint) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            timePoint.time_since_epoch())
            .count());
}
}  // namespace Util
