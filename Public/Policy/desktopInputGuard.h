#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace revia::policy
{

// The stop that does not depend on Revia being willing or able to stop.
//
// Desktop operation is the one capability where a wrong decision keeps producing
// consequences while it is being noticed, so the stop path deliberately avoids the
// model, the conversation runtime, and the queue an ordinary cancellation travels
// through. Two things trip it:
//
//   * an explicit call from the desktop shell, the CLI, or shutdown, and
//   * the physical panic hold -- ctrl, alt, and shift down together -- which the
//     executor samples immediately before it synthesizes anything.
//
// It latches. An emergency stop that clears itself is a pause, and the difference
// matters when the reason it was tripped has not been dealt with yet.
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
