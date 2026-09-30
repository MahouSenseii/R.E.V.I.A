#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace revia::policy
{

// Latched emergency stop bypasses the model and cancellation queues.
// Trips explicitly or when the executor samples Ctrl+Alt+Shift before input injection.
class DesktopInputGuard
{
public:
    void Trip(std::string reason);
    // Returns whether it had actually been tripped, so a caller can report the
    // difference between clearing a stop and doing nothing.
    bool Resume();
    [[nodiscard]] bool IsTripped() const;
    [[nodiscard]] std::string Reason() const;

    // Samples the physical panic hold and trips on it. Called before injection and
    // never during a synthesized chord, because Revia's own ctrl/alt/shift keystrokes
    // are indistinguishable from a person's at this layer and would self-trip.
    // Always false off Windows.
    bool CheckPhysicalStop();

private:
    std::atomic<bool> tripped{false};
    mutable std::mutex mutex;
    std::string reason;
};

} // namespace revia::policy
