#include "Core/Session.hpp"

namespace Core {
Session::Session(uint64_t sessionId, std::string remoteKey, Util::TimePoint now)
    : sessionId_(sessionId),
      remoteKey_(std::move(remoteKey)),
      lastHeard_(now),
      blocked_(false) {}

uint64_t Session::sessionId() const {   // sessionId_를 그대로 반환하는 아주 단순한 getter
    return sessionId_;
}

const std::string& Session::remoteKey() const {
    return remoteKey_;
}

Util::TimePoint Session::lastHeard() const {
    return lastHeard_;
}

void Session::updateLastHeard(Util::TimePoint now) {
    lastHeard_ = now;
}

bool Session::isExpired(Util::TimePoint now, std::chrono::milliseconds timeout) const {
    return (now - lastHeard_) > timeout;
}

bool Session::isBlocked() const {
    return blocked_;
}

void Session::setBlocked(bool blocked) {
    blocked_ = blocked;     // 멤버 변수는 blocked_, 함수인자는 blocked. 이름은 비슷하나 _로 구분하고 있음.
}
}  // namespace Core
