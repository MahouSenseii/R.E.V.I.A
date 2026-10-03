#pragma once

#include "Runtime/runtimeStamp.h"

#include <mutex>
#include <string>

namespace revia::runtime
{

class SessionIdentity
{
  public:
    explicit SessionIdentity(std::string companionId);

    [[nodiscard]] RuntimeStamp Stamp(std::string taskId = {}, std::string attemptId = {}, std::uint64_t policyVersion = 0) const;
    [[nodiscard]] bool IsCurrent(const RuntimeStamp& stamp) const;
    void BeginSession();
    void Invalidate();

  private:
    mutable std::mutex mutex;
    RuntimeStamp current;
    bool active = true;
};

} // namespace revia::runtime
