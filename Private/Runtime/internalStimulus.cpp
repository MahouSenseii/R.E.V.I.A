#include "Runtime/internalStimulus.h"

namespace revia::runtime
{

std::string ToString(const InternalEventKind kind)
{
    switch (kind)
    {
        case InternalEventKind::ActivitySucceeded: return "ActivitySucceeded";
        case InternalEventKind::ActivityFailed: return "ActivityFailed";
        case InternalEventKind::ActionRefused: return "ActionRefused";
        case InternalEventKind::DiscoveryMade: return "DiscoveryMade";
        case InternalEventKind::WaitEnded: return "WaitEnded";
    }
    return "ActivitySucceeded";
}

} // namespace revia::runtime
