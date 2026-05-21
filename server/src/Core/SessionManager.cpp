#include "Core/SessionManager.hpp"

namespace Core {
SessionManager::SessionManager(std::chrono::milliseconds timeout)
    : nextSessionId_(1), timeout_(timeout) {}

std::shared_ptr<Session> SessionManager::findOrCreate(
    const std::string& remoteKey,
    Util::TimePoint now) {
    auto it = sessions_.find(remoteKey);
    if (it != sessions_.end()) {
        return it->second;
    }

    if (!allowNewConnection(remoteKey)) {
        return nullptr;
    }

    auto session = std::make_shared<Session>(nextSessionId_++, remoteKey, now);
    sessions_.emplace(remoteKey, session);
    return session;
}

std::shared_ptr<Session> SessionManager::find(const std::string& remoteKey) const {
    auto it = sessions_.find(remoteKey);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<Session> SessionManager::findBySessionId(uint64_t sessionId) const {
    for (const auto& entry : sessions_) {
        if (entry.second->sessionId() == sessionId) {
            return entry.second;
        }
    }
    return nullptr;
}

void SessionManager::remove(const std::string& remoteKey) {
    sessions_.erase(remoteKey);
}

void SessionManager::tick(Util::TimePoint now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second->isExpired(now, timeout_)) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t SessionManager::size() const {
    return sessions_.size();
}

bool SessionManager::allowNewConnection(const std::string& /*remoteKey*/) const {
    return true;
}
}  // namespace Core
