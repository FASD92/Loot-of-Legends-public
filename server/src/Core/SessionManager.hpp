#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "Core/Session.hpp"
#include "Util/Time.hpp"

namespace Core {
class SessionManager {
public:
    explicit SessionManager(std::chrono::milliseconds timeout);

    std::shared_ptr<Session> findOrCreate(const std::string& remoteKey, Util::TimePoint now);
    std::shared_ptr<Session> find(const std::string& remoteKey) const;
    void remove(const std::string& remoteKey);

    void tick(Util::TimePoint now);
    size_t size() const;

private:
    bool allowNewConnection(const std::string& remoteKey) const;

    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    uint64_t nextSessionId_;
    std::chrono::milliseconds timeout_;
};
}  // namespace Core
