#pragma once

#include <chrono>
#include <cstdint>

namespace Util {    // Util 네임스페이스
using Clock = std::chrono::steady_clock;    // system_clock은 NTP 동기화 때문에 역행 또는 건너뛸 수 있어서, 단조 증가하는 steady_clock을 선정함
using TimePoint = Clock::time_point;    // TimePoint는 steady_clock 기준의 시간점 타입

TimePoint now();    // TimePoint를 반환하는 함수 now() 선언.
uint64_t toMillis(TimePoint timePoint);     // TimpePoint 타입 매개변수를 uint64_t 타입으로 반환하는 함수
}  // namespace Util
