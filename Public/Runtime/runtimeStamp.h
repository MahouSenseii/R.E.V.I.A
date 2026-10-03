#pragma once

#include <cstdint>
#include <string>

namespace revia::runtime
{

struct RuntimeStamp
{
    std::string companionId;
    std::string sessionId;
    std::uint64_t generation = 0;
    std::string taskId;
    std::string attemptId;
    std::uint64_t policyVersion = 0;

    [[nodiscard]] bool SameSession(const RuntimeStamp& other) const
    {
        return companionId == other.companionId && sessionId == other.sessionId && generation == other.generation;
    }
};

} // namespace revia::runtime
