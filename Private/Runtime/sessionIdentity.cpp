#include "Runtime/sessionIdentity.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace revia::runtime
{
namespace
{
std::string NextSessionId()
{
    static std::atomic<std::uint64_t> sequence = 0;
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return std::to_string(now) + "-" + std::to_string(++sequence);
}
}

SessionIdentity::SessionIdentity(std::string companionId)
{
    if (companionId.empty())
        throw std::invalid_argument("A companion identity is required.");
    current.companionId = std::move(companionId);
    current.sessionId = NextSessionId();
    current.generation = 1;
}

RuntimeStamp SessionIdentity::Stamp(std::string taskId, std::string attemptId, const std::uint64_t policyVersion) const
{
    std::lock_guard lock(mutex);
    RuntimeStamp result = current;
    result.taskId = std::move(taskId);
    result.attemptId = std::move(attemptId);
    result.policyVersion = policyVersion;
    return result;
}

bool SessionIdentity::IsCurrent(const RuntimeStamp& stamp) const
{
    std::lock_guard lock(mutex);
    return active && current.SameSession(stamp);
}

void SessionIdentity::BeginSession()
{
    std::lock_guard lock(mutex);
    current.sessionId = NextSessionId();
    ++current.generation;
    active = true;
}

void SessionIdentity::Invalidate()
{
    std::lock_guard lock(mutex);
    active = false;
}

} // namespace revia::runtime
