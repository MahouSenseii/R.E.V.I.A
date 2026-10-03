#include "Resources/runtimeLease.h"

#include <mutex>

namespace revia::resources
{
namespace
{
std::mutex leaseMutex;
const RuntimeLease* foreground = nullptr;
}

RuntimeLease::~RuntimeLease()
{
    Release();
}

bool RuntimeLease::TryAcquire(const runtime::RuntimeStamp& stamp)
{
    if (stamp.companionId.empty() || stamp.sessionId.empty())
        return false;
    std::lock_guard lock(leaseMutex);
    if (foreground != nullptr && foreground != this)
        return false;
    foreground = this;
    owner = stamp.sessionId;
    return true;
}

void RuntimeLease::Release()
{
    std::lock_guard lock(leaseMutex);
    if (foreground == this)
        foreground = nullptr;
    owner.clear();
}
}
