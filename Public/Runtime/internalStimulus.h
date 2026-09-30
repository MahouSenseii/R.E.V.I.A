#pragma once

#include <string>

namespace revia::runtime
{

// Typed internal outcomes must be confirmed by the runtime before producing affect.
// The model cannot invent events that change emotion.
enum class InternalEventKind
{
    // An activity Revia was running finished the way it was meant to.
    ActivitySucceeded,
    // It did not. Whether that stings depends on selfCaused below.
    ActivityFailed,
    // Policy said no. Distinct from failure: nothing broke, she was simply not allowed.
    ActionRefused,
    // Something turned up that she did not already know.
    DiscoveryMade,
    // Something she was waiting on is finally done.
    WaitEnded
};

[[nodiscard]] std::string ToString(InternalEventKind kind);

struct InternalStimulus
{
    InternalEventKind kind = InternalEventKind::ActivitySucceeded;
    // The component that confirmed it, e.g. "Goal runner". Carried so the resulting
    // feeling can say what caused it rather than appearing from nowhere.
    std::string source;
    std::string detail;
    // 0 is unmistakable success, 1 is total failure. Read only for the outcome kinds.
    float failure = 0.0F;
    // How much this mattered. Below the controller's floor nothing happens at all, and
    // that is the ordinary result rather than a missed case: a companion who visibly
    // reacts to every background success is not expressive, she is noisy.
    float importance = 0.5F;
    // How much of this was new. Novelty is what separates interest from mere
    // satisfaction, and surprise from mere disappointment.
    float novelty = 0.0F;
    // Whether Revia's own choice caused it. The same failure lands differently when she
    // picked the approach than when something external broke underneath her.
    bool selfCaused = false;
};

} // namespace revia::runtime
