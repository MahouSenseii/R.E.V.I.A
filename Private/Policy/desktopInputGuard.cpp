#include "Policy/desktopInputGuard.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::policy
{

void DesktopInputGuard::Trip(std::string newReason)
{
    {
        std::lock_guard lock(mutex);
        // The first reason is the useful one. A later trip while already stopped is
        // usually a consequence of the first, and overwriting would hide the cause.
        if (!tripped.load(std::memory_order_acquire) || reason.empty())
        {
            reason = newReason.empty() ? "Desktop control was stopped." : std::move(newReason);
        }
    }
    tripped.store(true, std::memory_order_release);
}

bool DesktopInputGuard::Resume()
{
    const bool wasTripped = tripped.exchange(false, std::memory_order_acq_rel);
    std::lock_guard lock(mutex);
    reason.clear();
    return wasTripped;
}

bool DesktopInputGuard::IsTripped() const
{
    return tripped.load(std::memory_order_acquire);
}

std::string DesktopInputGuard::Reason() const
{
    std::lock_guard lock(mutex);
    return reason;
}

bool DesktopInputGuard::CheckPhysicalStop()
{
#ifdef _WIN32
    const auto held = [](const int virtualKey)
    {
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    };
    if (held(VK_CONTROL) && held(VK_MENU) && held(VK_SHIFT))
    {
        Trip("The physical stop hold (ctrl+alt+shift) was pressed.");
        return true;
    }
#endif
    return IsTripped();
}

} // namespace revia::policy
