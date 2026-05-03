#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "Util/Time.hpp"

namespace Core {
class Session {
public:
    Session(uint64_t sessionId, std::string remoteKey, Util::TimePoint now);

    uint64_t sessionId() const;
    const std::string& remoteKey() const;   // 내부 문자열을 복사가 아닌 '참조'로 돌려준다.
    Util::TimePoint lastHeard() const;

    void updateLastHeard(Util::TimePoint now);
    bool isExpired(Util::TimePoint now, std::chrono::milliseconds timeout) const;

    bool isBlocked() const;
    void setBlocked(bool blocked);

private:
    uint64_t sessionId_;
    std::string remoteKey_;
    Util::TimePoint lastHeard_;
    bool blocked_;
};
}  // namespace Core
