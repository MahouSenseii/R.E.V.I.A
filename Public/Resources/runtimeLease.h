#pragma once

#include "Runtime/runtimeStamp.h"

#include <string>

namespace revia::resources
{

// One foreground runtime owns model-process and physical-device lifetimes in this host.
class RuntimeLease
{
  public:
    ~RuntimeLease();
    RuntimeLease() = default;
    RuntimeLease(const RuntimeLease&) = delete;
    RuntimeLease& operator=(const RuntimeLease&) = delete;

    bool TryAcquire(const runtime::RuntimeStamp& stamp);
    void Release();

  private:
    std::string owner;
};
}
